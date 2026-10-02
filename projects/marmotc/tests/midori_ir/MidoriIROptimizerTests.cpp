#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MidoriIR/Builder/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/Printer/MidoriIRPrinter.h"
#include "Compiler/MidoriIROptimizer/MidoriIROptimizer.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"
#include "support/CompileHelpers.h"

#include <algorithm>
#include <bit>
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
		const std::expected<void, MidoriIRPassFailure> optimized = sizeof...(Passes) == 0u
			? MidoriIROptimizer().Optimize(module)
			: MidoriIROptimizer(std::move(passes)).Optimize(module);
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

		bool Run(MidoriIRModule& module) const override
		{
			module.m_functions.front().m_blocks.front().m_instructions.pop_back();
			return true;
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

TEST_CASE("Sccp follows dependencies between globals in the same module", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SccpPass, DeadCodeEliminationPass>(R"(module GlobalChain
def first = 2;
def second = first + 3;
def third = second * 2;
def Scale = fn(x: Int) -> Int => x + third;
Scale(4);
)");

	CHECK(PrintFunction(module, "Scale") == R"(fn Scale(Int) -> Int
bb0(x: Int):
  third: Int = Const 10
  %2: Int = AddInt x, third
  return %2
)");
}

TEST_CASE("The optimizer revisits known closures and constants exposed by earlier rewrites", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<>(R"(module Revisit
def Apply = fn(f: fn(Int) -> Int, x: Int) -> Int => f(x);
def HigherOrder = fn(x: Int) -> Int => Apply(fn(y: Int) -> Int => y + 1, x);
def Cascade = fn(x: Int) -> Int => ((x * 0) + 5) * 2;
HigherOrder(3);
Cascade(3);
)");

	const std::string higher_order = PrintFunction(module, "HigherOrder");
	CHECK(higher_order.find("MakeClosure") == std::string::npos);
	CHECK(higher_order.find("CallValue") == std::string::npos);
	CHECK(higher_order.find("tailcall") == std::string::npos);
	CHECK(higher_order.find("AddInt") != std::string::npos);
	const std::string cascade = PrintFunction(module, "Cascade");
	CHECK(cascade.find("Const 10") != std::string::npos);
	CHECK(cascade.find("AddInt") == std::string::npos);
	CHECK(cascade.find("MulInt") == std::string::npos);
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

TEST_CASE("DeadCodeElimination preserves faults while compacting instruction definitions", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<DeadCodeEliminationPass>(R"(module Faults
def Bad = fn(x: Int) -> Int => {
	def unused = x + 1;
	def zero = 0;
	def a = 2;
	def b = 3;
	def bad = x / zero;
	a + b
};
Bad(7);
)");

	CHECK(PrintFunction(module, "Bad") == R"(fn Bad(Int) -> Int
bb0(x: Int):
  %3: Int = Const 0
  %4: Int = Const 2
  %5: Int = Const 3
  %6: Int = DivInt x, %3  !fault(DivisionByZero)
  %7: Int = AddInt %4, %5
  return %7
)");
}

TEST_CASE("DeadCodeElimination drops the test an exhaustive match leaves for its last arm", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<DeadCodeEliminationPass>(R"(module Exhaustive
type Opt = None | Some(Int);
def Get = fn(o: Opt) -> Int => match o with
	case Opt::Some(v) => v
	case Opt::None => 0;
Get(Opt::Some(3));
)");

	CHECK(PrintFunction(module, "Get") == R"(fn Get(Exhaustive::Opt) -> Int
bb0(o: Exhaustive::Opt):
  %1: Int = GetTag o
  %2: Int = Const 1
  %3: Bool = EqInt %1, %2
  branch %3, bb1, bb2
bb1:
  %4: Int = UnionField tag 1 #0, o
  return %4
bb2:
  %8: Int = Const 0
  return %8
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

TEST_CASE("StrengthReduction applies identities to Byte and Word at their own widths", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SccpPass, StrengthReductionPass, DeadCodeEliminationPass>(R"(module UnsignedIdentities
def Bytes = fn(x: Byte) -> Byte => ((x + 0x00) * 0x01) & 0xFF;
def Words = fn(x: Word) -> Word => ((x + (0 as Word)) * (1 as Word)) & 0xFFFFFFFFFFFFFFFF;
def ZeroByte = fn(x: Byte) -> Byte => x - x;
def ZeroWord = fn(x: Word) -> Word => x % (1 as Word);
Bytes(0xFF);
Words(0xFFFFFFFFFFFFFFFF);
ZeroByte(0xFF);
ZeroWord(0xFFFFFFFFFFFFFFFF);
)");

	CHECK(PrintFunction(module, "Bytes") == "fn Bytes(Byte) -> Byte\nbb0(x: Byte):\n  return x\n");
	CHECK(PrintFunction(module, "Words") == "fn Words(Word) -> Word\nbb0(x: Word):\n  return x\n");
	CHECK(PrintFunction(module, "ZeroByte").find(": Byte = Const 0") != std::string::npos);
	CHECK(PrintFunction(module, "ZeroWord").find(": Word = Const 0") != std::string::npos);
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
  jump bb3(%3)
bb2:
  %4: Mixed::Opt = Call Make, n  !call
  %6: Int = GetTag %4
  %7: Int = Const 1
  %8: Bool = EqInt %6, %7
  branch %8, bb3(%4), bb4
bb3(%19: Mixed::Opt):
  %9: Int = UnionField tag 1 #0, %19
  %10: Int = Const 1
  %11: Int = AddInt %9, %10
  return %11
bb4:
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

TEST_CASE("GlobalValueNumbering shares scalar literals when numbering their users", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<GlobalValueNumberingPass, DeadCodeEliminationPass>(R"(module Literals
def Twice = fn(x: Int) -> Int => (x + 1) + (x + 1);
Twice(3);
)");

	CHECK(PrintFunction(module, "Twice") == R"(fn Twice(Int) -> Int
bb0(x: Int):
  %1: Int = Const 1
  %2: Int = AddInt x, %1
  %5: Int = AddInt %2, %2
  return %5
)");
}

TEST_CASE("GlobalValueNumbering follows a branch's truth only within its dominated path", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<GlobalValueNumberingPass, DeadCodeEliminationPass>(R"(module BranchFacts
def Both = fn(x: Int) -> Int =>
	if x == 0 then (if x == 0 then 10 else 20) else (if x == 0 then 30 else 40);
def Nested = fn(x: Int, choose: Bool) -> Int =>
	if x == 0 then (if choose then (if x == 0 then 10 else 20) else 30) else 40;
def Join = fn(flag: Bool) -> Int => {
	def offset = if flag then 1 else 2;
	offset + (if flag then 3 else 4)
};
def ReadAgain = fn(cell: Ref<Bool>) -> Int =>
	if *cell then { cell := false; if *cell then 3 else 4 } else 5;
Both(0);
Nested(0, true);
Join(true);
ReadAgain(ref true);
)");

	const std::string both = PrintFunction(module, "Both");
	const size_t branch = both.find("branch");
	REQUIRE(branch != std::string::npos);
	CHECK(both.find("branch", branch + 1u) == std::string::npos);
	CHECK(both.find("Const 20") == std::string::npos);
	CHECK(both.find("Const 30") == std::string::npos);
	CHECK(both.find("Const 10") != std::string::npos);
	CHECK(both.find("Const 40") != std::string::npos);
	CHECK(PrintFunction(module, "Nested").find("Const 20") == std::string::npos);
	for (const std::string_view name : { "Join", "ReadAgain" })
	{
		const std::string function = PrintFunction(module, name);
		const size_t first = function.find("branch");
		REQUIRE(first != std::string::npos);
		CHECK(function.find("branch", first + 1u) != std::string::npos);
	}
}

TEST_CASE("GlobalValueNumbering does not assume a condition when both branch edges enter one block", "[midori_ir][optimizer]")
{
	MidoriIRModule module("SharedSuccessor");
	MidoriIRFunction function("Pick", MidoriIRScalarType(MidoriIRScalar::Int));
	MidoriIRBuilder builder(function);
	const MidoriIRBlockId join = builder.CreateBlock();
	const MidoriIRBlockId yes = builder.CreateBlock();
	const MidoriIRBlockId no = builder.CreateBlock();
	const MidoriIRValueId flag = builder.AddParameter(MidoriIRFunction::s_entry_block, MidoriIRScalarType(MidoriIRScalar::Bool), "flag");
	const MidoriIRValueId one = builder.ConstInt(1);
	const MidoriIRValueId two = builder.ConstInt(2);
	builder.Branch(flag, MidoriIRSuccessor(join, { one }), MidoriIRSuccessor(join, { two }));
	const MidoriIRValueId selected = builder.AddParameter(join, MidoriIRScalarType(MidoriIRScalar::Int));
	builder.PositionAt(join).Branch(flag, MidoriIRSuccessor(yes), MidoriIRSuccessor(no));
	builder.PositionAt(yes).Return(selected);
	builder.PositionAt(no).Return(two);
	module.AddFunction(std::move(function));
	REQUIRE(MidoriIRVerifier(module).Verify().empty());
	CHECK_FALSE(GlobalValueNumberingPass().Run(module));
	CHECK(module.m_functions.front().Block(join).m_instructions.back().m_op == MidoriIROp::Branch);
	CHECK(MidoriIRVerifier(module).Verify().empty());
}

TEST_CASE("GlobalValueNumbering canonicalizes unsigned commutative operations", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<GlobalValueNumberingPass>(R"(module UnsignedNumbering
def Bytes = fn(x: Byte, y: Byte) -> Byte => (x + y) ^ (y + x);
def Words = fn(x: Word, y: Word) -> Word => (x * y) ^ (y * x);
Bytes(0x01, 0x02);
Words(1 as Word, 2 as Word);
)");
	for (const auto& [name, operation] : { std::pair{ "Bytes", "AddByte" }, std::pair{ "Words", "MulWord" } })
	{
		const std::string function = PrintFunction(module, name);
		const size_t first = function.find(operation);
		REQUIRE(first != std::string::npos);
		CHECK(function.find(operation, first + 1u) == std::string::npos);
	}
}

TEST_CASE("GlobalValueNumbering removes a fault only after the same computation dominates it", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<GlobalValueNumberingPass>(R"(module FaultNumbering
def Twice = fn(x: Int, y: Int) -> Int => (x / y) + (x / y);
def Either = fn(x: Int, y: Int, choose: Bool) -> Int => if choose then x / y else x / y;
Twice(3, 2);
Either(3, 2, true);
)");

	CHECK(PrintFunction(module, "Twice") == R"(fn Twice(Int, Int) -> Int
bb0(x: Int, y: Int):
  %2: Int = DivInt x, y  !fault(DivisionByZero)
  %4: Int = AddInt %2, %2
  return %4
)");
	const std::string either = PrintFunction(module, "Either");
	const size_t first = either.find("DivInt");
	REQUIRE(first != std::string::npos);
	CHECK(either.find("DivInt", first + 1u) != std::string::npos);
}

TEST_CASE("GlobalValueNumbering preserves distinct float zeros and text allocations", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SccpPass, GlobalValueNumberingPass, DeadCodeEliminationPass>(R"(module Identities
def Zeros = fn(x: Float) -> (Float, Float) => (x * 0.0, x * (0.0 * -1.0));
def Texts = fn() -> (Text, Text) => ("same", "same");
Zeros(2.0);
Texts();
)");

	const std::string zeros = PrintFunction(module, "Zeros");
	const size_t first_multiply = zeros.find("MulFloat");
	REQUIRE(first_multiply != std::string::npos);
	CHECK(zeros.find("MulFloat", first_multiply + 1u) != std::string::npos);
	const std::string texts = PrintFunction(module, "Texts");
	const size_t first_text = texts.find("Const \"same\"");
	REQUIRE(first_text != std::string::npos);
	CHECK(texts.find("Const \"same\"", first_text + 1u) != std::string::npos);
}

TEST_CASE("GlobalValueNumbering compares NaN constants by their payload bits", "[midori_ir][optimizer]")
{
	const std::shared_ptr<MidoriType> float_type = MidoriIRScalarType(MidoriIRScalar::Float);
	const std::shared_ptr<MidoriType> tuple_type = MidoriType::MakeTupleType({ float_type, float_type, float_type });
	MidoriIRModule module("NaNPayloads");
	MidoriIRFunction function("Values", tuple_type);
	MidoriIRBuilder builder(function);
	const double nan = std::bit_cast<double>(uint64_t{ 0x7ff8000000000001 });
	const double other_nan = std::bit_cast<double>(uint64_t{ 0x7ff8000000000002 });
	const MidoriIRValueId first = builder.ConstFloat(nan);
	const MidoriIRValueId same = builder.ConstFloat(nan);
	const MidoriIRValueId other = builder.ConstFloat(other_nan);
	builder.Return(builder.Emit(MidoriIROp::MakeTuple, tuple_type, { first, same, other }));
	module.AddFunction(std::move(function));
	REQUIRE(MidoriIRVerifier(module).Verify().empty());
	CHECK(GlobalValueNumberingPass().Run(module));
	CHECK(MidoriIRVerifier(module).Verify().empty());
	const std::vector<MidoriIRInstruction>& instructions = module.m_functions.front().m_blocks.front().m_instructions;
	REQUIRE(instructions.size() == 4u);
	const MidoriIRInstruction& tuple = instructions[2u];
	CHECK(tuple.m_operands[0u] == tuple.m_operands[1u]);
	CHECK(tuple.m_operands[0u] != tuple.m_operands[2u]);
	CHECK_FALSE(GlobalValueNumberingPass().Run(module));
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

TEST_CASE("LoopInvariantCodeMotion hoists proven non-faulting division and leaves a variable divisor", "[midori_ir][optimizer]")
{
	const MidoriIRModule module = Optimize<SelfTailCallPass, DeadCodeEliminationPass, SccpPass, LoopInvariantCodeMotionPass>(R"(module SafeHoist
def Safe = fn(n: Int, x: Int, sum: Int) -> Int => if n <= 0 then sum else Safe(n - 1, x, sum + (x / 3));
def MayFail = fn(n: Int, x: Int, d: Int, sum: Int) -> Int => if n <= 0 then sum else MayFail(n - 1, x, d, sum + (x / d));
Safe(3, 12, 0);
MayFail(0, 12, 0, 0);
)");

	const std::string safe = PrintFunction(module, "Safe");
	CHECK(safe.find("DivInt") < safe.find("jump bb1"));
	CHECK(safe.find("fault(DivisionByZero)") == std::string::npos);
	const std::string may_fail = PrintFunction(module, "MayFail");
	CHECK(may_fail.find("DivInt") > may_fail.find("jump bb1"));
	CHECK(may_fail.find("fault(DivisionByZero)") != std::string::npos);
}

TEST_CASE("The optimizer names the pass that left the module invalid", "[midori_ir][optimizer]")
{
	if (!MidoriIROptimizer::VerifiesEachPass())
	{
		SKIP("only Dev and Debug builds verify after each pass");
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
