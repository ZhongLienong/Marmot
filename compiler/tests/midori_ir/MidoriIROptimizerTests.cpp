#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MidoriIR/Builder/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/Printer/MidoriIRPrinter.h"
#include "Compiler/MidoriIROptimizer/MidoriIROptimizer.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"
#include "support/CompileHelpers.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	template<typename... Passes>
	MidoriIRModule Optimize(std::string source)
	{
		std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::move(source));
		if (!lowered.has_value())
		{
			FAIL(lowered.error().Rendered());
		}
		std::vector<std::unique_ptr<MidoriIRPass>> passes;
		(passes.push_back(std::make_unique<Passes>()), ...);
		MidoriIRModule module = std::move(lowered->m_module);
		const std::expected<void, MidoriIRPassFailure> optimized = MidoriIROptimizer(std::move(passes)).Optimize(module);
		if (!optimized.has_value())
		{
			for (const MidoriIRViolation& violation : optimized.error().m_violations)
			{
				UNSCOPED_INFO(violation.ToString());
			}
			FAIL("the pass " + optimized.error().m_pass + " left invalid MidoriIR");
		}
		REQUIRE(MidoriIRVerifier(module).Verify().empty());
		return module;
	}

	std::string PrintFunction(const MidoriIRModule& module, std::string_view name)
	{
		const std::vector<MidoriIRFunction>::const_iterator function = std::ranges::find_if(module.m_functions, [name](const MidoriIRFunction& candidate) { return candidate.m_name.starts_with(name); });
		REQUIRE(function != module.m_functions.end());
		return MidoriIRPrinter(module).PrintFunction(*function);
	}

	// Leaves the module without a terminator in its first function.
	class BreakingPass : public MidoriIRPass
	{
	public:
		std::string_view Name() const override
		{
			return "Breaking";
		}

		void Run(MidoriIRModule& module) const override
		{
			module.m_functions.front().m_blocks.front().m_instructions.pop_back();
		}
	};
}

TEST_CASE("Sccp folds constants, branches on them and reads a global defined by one", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SccpPass, DeadCodeEliminationPass>(R"(module Folding
def limit = 4 * 25;
def Scale = fn(x: Int) -> Int => {
	def factor = limit / 10;
	if factor > 5 then x * factor else x - factor
};
Scale(3);
)");

	CHECK(PrintFunction(module, "Scale") == R"(fn Scale(Int) -> Int
bb0(x: Int):
  %3: Int = Const 10
  %6: Int = MulInt x, %3
  return %6
)");
}

TEST_CASE("DeadCodeElimination drops unused values and joins the blocks a constant branch leaves", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<DeadCodeEliminationPass>(R"(module Dead
def Pick = fn(x: Int) -> Int => {
	def unused = x * 2;
	def pair = (x, unused);
	if true then x + 1 else x - 1
};
Pick(3);
)");

	CHECK(PrintFunction(module, "Pick") == R"(fn Pick(Int) -> Int
bb0(x: Int):
  %5: Int = Const 1
  %6: Int = AddInt x, %5
  return %6
)");
}

TEST_CASE("SelfTailCall turns a tail call of the function itself into a loop", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass>(R"(module Countdown
def Count = fn(n: Int, total: Int) -> Int => if n == 0 then total else Count(n - 1, total + n);
Count(3, 0);
)");

	CHECK(PrintFunction(module, "Count") == R"(fn Count(Int, Int) -> Int
bb0(n.0: Int, total.1: Int):
  jump bb1(n.0, total.1)
bb1(n.7: Int, total.8: Int):
  %2: Int = Const 0
  %3: Bool = EqInt n.7, %2
  branch %3, bb2, bb3
bb2:
  return total.8
bb3:
  %4: Int = Const 1
  %5: Int = SubInt n.7, %4
  %6: Int = AddInt total.8, n.7
  jump bb1(%5, %6)
)");
}

TEST_CASE("ClosureConversion passes a local function's captures as parameters once its closure is only called", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass, ClosureConversionPass, DeadCodeEliminationPass>(R"(module Lift
def Sum = fn(n: Int) -> Int => {
	def Go = fn(i: Int, total: Int) -> Int => if i > n then total else Go(i + 1, total + i);
	Go(1, 0)
};
Sum(4);
)");

	CHECK(PrintFunction(module, "Sum") == R"(fn Sum(Int) -> Int
bb0(n: Int):
  %3: Int = Const 1
  %4: Int = Const 0
  tailcall Anonymous Function at line: 3, %3, %4, n
)");
	CHECK(PrintFunction(module, "Anonymous") == R"(fn Anonymous Function at line: 3(Int, Int, Int) -> Int
bb0(i.0: Int, total.1: Int, n: Int):
  jump bb1(i.0, total.1)
bb1(i.8: Int, total.9: Int):
  %3: Bool = GtInt i.8, n
  branch %3, bb2, bb3
bb2:
  return total.9
bb3:
  %4: Int = Const 1
  %5: Int = AddInt i.8, %4
  %6: Int = AddInt total.9, i.8
  jump bb1(%5, %6)
)");
}

TEST_CASE("Contification makes a local function only ever tail called blocks of its caller", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass, ClosureConversionPass, ContificationPass, DeadCodeEliminationPass>(R"(module Contify
def Sum = fn(n: Int) -> Int => {
	def Go = fn(i: Int, total: Int) -> Int => if i > n then total else Go(i + 1, total + i);
	if n < 0 then Go(0, 0) else Go(1, 0)
};
Sum(4);
)");

	CHECK(PrintFunction(module, "Sum") == R"(fn Sum(Int) -> Int
bb0(n.0: Int):
  %3: Int = Const 0
  %4: Bool = LtInt n.0, %3
  branch %4, bb1, bb2
bb1:
  %5: Int = Const 0
  %6: Int = Const 0
  jump bb3(%5, %6)
bb2:
  %7: Int = Const 1
  %8: Int = Const 0
  jump bb3(%7, %8)
bb3(i.12: Int, total.13: Int):
  %14: Bool = GtInt i.12, n.0
  branch %14, bb4, bb5
bb4:
  return total.13
bb5:
  %15: Int = Const 1
  %16: Int = AddInt i.12, %15
  %17: Int = AddInt total.13, i.12
  jump bb3(%16, %17)
)");
}

TEST_CASE("Inlining copies a small callee into its caller", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<InliningPass, DeadCodeEliminationPass>(R"(module Inline
def Square = fn(x: Int) -> Int => x * x;
def Area = fn(w: Int) -> Int => Square(w) + 1;
Area(3);
)");

	CHECK(PrintFunction(module, "Area") == R"(fn Area(Int) -> Int
bb0(w: Int):
  %5: Int = MulInt w, w
  %2: Int = Const 1
  %3: Int = AddInt %5, %2
  return %3
)");
}

TEST_CASE("Inlining keeps a call in tail position a tail call", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<InliningPass, DeadCodeEliminationPass>(R"(module TailInline
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
def Report = fn(x: Int) -> Int => { Print(x as Text); x };
def Double = fn(x: Int) -> Int => Report(x * 2);
def Twice = fn(x: Int) -> Int => Double(x + 1);
def Later = fn(x: Int) -> Int => Double(x) + 1;
Twice(3) + Later(3);
)");

	CHECK(PrintFunction(module, "Twice") == R"(fn Twice(Int) -> Int
bb0(x.0: Int):
  %1: Int = Const 1
  %2: Int = AddInt x.0, %1
  %4: Int = Const 2
  %5: Int = MulInt %2, %4
  tailcall Report, %5
)");
	CHECK(PrintFunction(module, "Later") == R"(fn Later(Int) -> Int
bb0(x.0: Int):
  %5: Int = Const 2
  %6: Int = MulInt x.0, %5
  %7: Int = Call Report, %6  !call
  %2: Int = Const 1
  %3: Int = AddInt %7, %2
  return %3
)");
}

TEST_CASE("Inlining leaves a callee that can fail where a stack trace would show it", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<InliningPass, DeadCodeEliminationPass>(R"(module Traced
def Divide = fn(total: Int, parts: Int) -> Int => total / parts;
def Halve = fn(total: Int) -> Int => total / 2;
def Use = fn(x: Int) -> Int => Divide(x, x) + Halve(x);
Use(3);
)");

	CHECK(PrintFunction(module, "Use") == R"(fn Use(Int) -> Int
bb0(x: Int):
  %1: Int = Call Divide, x, x  !call
  %5: Int = Const 2
  %6: Int = DivInt x, %5  !fault(DivisionByZero)
  %3: Int = AddInt %1, %6
  return %3
)");
}

TEST_CASE("StrengthReduction applies only identities that hold for every value", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<StrengthReductionPass, DeadCodeEliminationPass>(R"(module Reduce
def Ints = fn(x: Int) -> Int => ((x * 8) + (x * 1)) + (0 - x);
def Floats = fn(y: Float) -> Float => ((y * 1.0) - 0.0) + ((y + 0.0) * 0.0);
Ints(3) + (Floats(2.0) as Int);
)");

	CHECK(PrintFunction(module, "Ints") == R"(fn Ints(Int) -> Int
bb0(x: Int):
  %9: Int = Const 3
  %2: Int = ShlInt x, %9
  %5: Int = AddInt %2, x
  %7: Int = NegInt x
  %8: Int = AddInt %5, %7
  return %8
)");
	CHECK(PrintFunction(module, "Floats") == R"(fn Floats(Float) -> Float
bb0(y: Float):
  %5: Float = Const 0.0
  %6: Float = AddFloat y, %5
  %7: Float = Const 0.0
  %8: Float = MulFloat %6, %7
  %9: Float = AddFloat y, %8
  return %9
)");
}

TEST_CASE("ScalarReplacement reads the parts of a struct made in the same function from what made it", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<ScalarReplacementPass, DeadCodeEliminationPass>(R"(module Scalars
type Point = { x : Int, y : Int };
def SumOf = fn(a: Int) -> Int => {
	def p = Point(a, a + 1);
	p.x + p.y
};
SumOf(3);
)");

	CHECK(PrintFunction(module, "SumOf") == R"(fn SumOf(Int) -> Int
bb0(a: Int):
  %1: Int = Const 1
  %2: Int = AddInt a, %1
  %6: Int = AddInt a, %2
  return %6
)");
}

TEST_CASE("KnownConstructorThreading sends a union made for a match straight to the arm its tag picks", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<KnownConstructorThreadingPass, DeadCodeEliminationPass, ScalarReplacementPass, DeadCodeEliminationPass>(R"(module Thread
type Opt = None | Some(Int);
def Pick = fn(n: Int) -> Int => {
	def o = if n > 0 then Opt::Some(n) else Opt::None();
	match o with
		case Opt::Some(v) => v + 1
		case Opt::None => 0
};
Pick(3);
)");

	CHECK(PrintFunction(module, "Pick") == R"(fn Pick(Int) -> Int
bb0(n: Int):
  %1: Int = Const 0
  %2: Bool = GtInt n, %1
  branch %2, bb1, bb2
bb1:
  %10: Int = Const 1
  %11: Int = AddInt n, %10
  return %11
bb2:
  %15: Int = Const 0
  return %15
)");
}

TEST_CASE("KnownConstructorThreading leaves a union read after the match is done with it", "[midori_ir][optimizer]")
{
	constexpr std::string_view source = R"(module Escape
type Opt = None | Some(Int);
def Weigh = fn(o: Opt) -> Int => match o with
	case Opt::Some(v) => v
	case Opt::None => 0
;
def Pick = fn(n: Int) -> Int => {
	def o = if n > 0 then Opt::Some(n) else Opt::None();
	def r = match o with
		case Opt::Some(v) => v + 1
		case Opt::None => 0
	;
	r + Weigh(o)
};
Pick(3);
)";

	CHECK(PrintFunction(Optimize<KnownConstructorThreadingPass, DeadCodeEliminationPass>(std::string(source)), "Pick") == PrintFunction(Optimize<DeadCodeEliminationPass>(std::string(source)), "Pick"));
}

TEST_CASE("KnownConstructorThreading leaves a block that does more than look", "[midori_ir][optimizer]")
{
	constexpr std::string_view source = R"(module Effect
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
type Opt = None | Some(Int);
def Pick = fn(n: Int) -> Int => {
	def o = if n > 0 then Opt::Some(n) else Opt::None();
	Print("matching");
	match o with
		case Opt::Some(v) => v + 1
		case Opt::None => 0
};
Pick(3);
)";

	CHECK(PrintFunction(Optimize<KnownConstructorThreadingPass, DeadCodeEliminationPass>(std::string(source)), "Pick") == PrintFunction(Optimize<DeadCodeEliminationPass>(std::string(source)), "Pick"));
}

TEST_CASE("KnownConstructorThreading threads only the edge whose union it knows", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<KnownConstructorThreadingPass, DeadCodeEliminationPass>(R"(module Mixed
type Opt = None | Some(Int);
def Make = fn(n: Int) -> Opt => if n == 0 then Opt::None() else Opt::Some(n);
def Pick = fn(n: Int) -> Int => {
	def o = if n > 0 then Opt::Some(n) else Make(n);
	match o with
		case Opt::Some(v) => v + 1
		case Opt::None => 0
};
Pick(3);
)");

	CHECK(PrintFunction(module, "Pick") == R"(fn Pick(Int) -> Int
bb0(n: Int):
  %1: Int = Const 0
  %2: Bool = GtInt n, %1
  branch %2, bb1, bb2
bb1:
  %3: Mixed::Opt = MakeUnion tag 1, n  !alloc
  jump bb4(%3)
bb2:
  %4: Mixed::Opt = Call Make, n  !call
  %6: Int = GetTag %4
  %7: Int = Const 1
  %8: Bool = EqInt %6, %7
  branch %8, bb4(%4), bb3
bb3:
  %12: Int = GetTag %4
  %13: Int = Const 0
  %14: Bool = EqInt %12, %13
  branch %14, bb6, bb5
bb4(%19: Mixed::Opt):
  %9: Int = UnionField tag 1 #0, %19
  %10: Int = Const 1
  %11: Int = AddInt %9, %10
  return %11
bb5:
  unreachable
bb6:
  %15: Int = Const 0
  return %15
)");
}

TEST_CASE("ParameterUnboxing carries a loop's struct as its members", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass, InliningPass, DeadCodeEliminationPass, ParameterUnboxingPass, DeadCodeEliminationPass>(R"(module Unbox
type Cursor = { at: Int, total: Int };
def Walk = fn(c: Cursor, n: Int) -> Int => if c.at == n then c.total else Walk(Cursor(c.at + 1, c.total + c.at), n);
def Sum = fn(n: Int) -> Int => Walk(Cursor(0, 0), n);
Sum(4);
)");

	CHECK(PrintFunction(module, "Sum") == R"(fn Sum(Int) -> Int
bb0(n.0: Int):
  %1: Int = Const 0
  %2: Int = Const 0
  jump bb1(%1, %2)
bb1(c.18: Int, c.19: Int):
  %9: Bool = EqInt c.18, n.0
  branch %9, bb2, bb3
bb2:
  return c.19
bb3:
  %12: Int = Const 1
  %13: Int = AddInt c.18, %12
  %16: Int = AddInt c.19, c.18
  jump bb1(%13, %16)
)");
}

TEST_CASE("ParameterUnboxing leaves a struct the loop passes on whole", "[midori_ir][optimizer]")
{
	constexpr std::string_view source = R"(module Escape
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
type Cursor = { at: Int, total: Int };
def Report = fn(c: Cursor) -> Unit => Print(c.at as Text);
def Walk = fn(c: Cursor, n: Int) -> Int => {
	Report(c);
	if c.at == n then c.total else Walk(Cursor(c.at + 1, c.total + c.at), n)
};
def Sum = fn(n: Int) -> Int => Walk(Cursor(0, 0), n);
Sum(4);
)";

	CHECK(PrintFunction(Optimize<SelfTailCallPass, InliningPass, DeadCodeEliminationPass, ParameterUnboxingPass, DeadCodeEliminationPass>(std::string(source)), "Sum") == PrintFunction(Optimize<SelfTailCallPass, InliningPass, DeadCodeEliminationPass, DeadCodeEliminationPass>(std::string(source)), "Sum"));
}

TEST_CASE("ParameterUnboxing leaves a struct that one edge passes without making it", "[midori_ir][optimizer]")
{
	constexpr std::string_view source = R"(module Mixed
type Cursor = { at: Int, total: Int };
def Walk = fn(c: Cursor, n: Int) -> Int => if c.at == n then c.total else Walk(Cursor(c.at + 1, c.total + c.at), n);
Walk(Cursor(0, 0), 4);
)";

	CHECK(PrintFunction(Optimize<SelfTailCallPass, DeadCodeEliminationPass, ParameterUnboxingPass, DeadCodeEliminationPass>(std::string(source)), "Walk") == PrintFunction(Optimize<SelfTailCallPass, DeadCodeEliminationPass, DeadCodeEliminationPass>(std::string(source)), "Walk"));
}

TEST_CASE("GlobalValueNumbering computes a value once where a dominating instruction already has", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<GlobalValueNumberingPass, DeadCodeEliminationPass>(R"(module Numbering
def Twice = fn(a: Int, b: Int) -> Int => if a > 0 then (a * b) + (b * a) else a * b;
Twice(3, 4);
)");

	CHECK(PrintFunction(module, "Twice") == R"(fn Twice(Int, Int) -> Int
bb0(a: Int, b: Int):
  %2: Int = Const 0
  %3: Bool = GtInt a, %2
  branch %3, bb1, bb2
bb1:
  %4: Int = MulInt a, b
  %6: Int = AddInt %4, %4
  return %6
bb2:
  %7: Int = MulInt a, b
  return %7
)");
}

TEST_CASE("LoopInvariantCodeMotion moves what a loop does not change to before the loop", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass, DeadCodeEliminationPass, LoopInvariantCodeMotionPass>(R"(module Hoist
def Count = fn(n: Int, k: Int, total: Int) -> Int => if n == 0 then total else Count(n - 1, k, total + (k * k));
Count(3, 2, 0);
)");

	CHECK(PrintFunction(module, "Count") == R"(fn Count(Int, Int, Int) -> Int
bb0(n.0: Int, k.1: Int, total.2: Int):
  %3: Int = Const 0
  %5: Int = Const 1
  %7: Int = MulInt k.1, k.1
  jump bb1(n.0, total.2)
bb1(n.9: Int, total.11: Int):
  %4: Bool = EqInt n.9, %3
  branch %4, bb2, bb3
bb2:
  return total.11
bb3:
  %6: Int = SubInt n.9, %5
  %8: Int = AddInt total.11, %7
  jump bb1(%6, %8)
)");
}

TEST_CASE("The optimizer names the pass that left the module invalid", "[midori_ir][optimizer]")
{
	if (!MidoriIROptimizer::VerifiesEachPass())
	{
		SKIP("only Development and Debug builds verify after each pass");
	}
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics("module Broken\ndef value = 1;\n");
	REQUIRE(lowered.has_value());
	std::vector<std::unique_ptr<MidoriIRPass>> passes;
	passes.push_back(std::make_unique<DeadCodeEliminationPass>());
	passes.push_back(std::make_unique<BreakingPass>());
	const std::expected<void, MidoriIRPassFailure> optimized = MidoriIROptimizer(std::move(passes)).Optimize(lowered->m_module);
	REQUIRE_FALSE(optimized.has_value());
	CHECK(optimized.error().m_pass == "Breaking");
	CHECK_FALSE(optimized.error().m_violations.empty());
}
