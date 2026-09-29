#include <catch2/catch_test_macros.hpp>

#include "Utility/TestMode/TestMode.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MidoriIR/Printer/MidoriIRPrinter.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
	LoweredModule RequireLowered(std::string source)
	{
		std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::move(source));
		if (!lowered.has_value())
		{
			FAIL(lowered.error().Rendered());
		}
		const std::vector<MidoriIRViolation> violations = MidoriIRVerifier(lowered->m_module).Verify();
		for (const MidoriIRViolation& violation : violations)
		{
			UNSCOPED_INFO(violation.ToString());
		}
		REQUIRE(violations.empty());
		return std::move(lowered).value();
	}

	CompilerError RequireLoweringError(std::string source)
	{
		std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::move(source));
		REQUIRE_FALSE(lowered.has_value());
		REQUIRE(lowered.error().Size() == 1u);
		return lowered.error().m_errors.front();
	}

	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("Lowering names a top-level function directly and turns a call in tail position into a tail call", "[midori_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Countdown
def Count = fn(n: Int, total: Int) -> Int => if n == 0 then total else Count(n - 1, total + n);
def start = 3;
Count(start, 0);
)");

	CHECK(MidoriIRPrinter(lowered.m_module).Print() == R"(module Countdown
global @0 Count: fn(Int, Int) -> Int
global @1 start: Int
top-level $main$

fn $main$() -> Unit
bb0:
  Count: fn(Int, Int) -> Int = MakeClosure Count  !alloc
  %1: Unit = GlobalDefine @0, Count  !io
  %2: Int = Const 3
  %3: Unit = GlobalDefine @1, %2  !io
  start: Int = GlobalGet @1  !read
  %5: Int = Const 0
  %6: Int = Call Count, start, %5  !call
  %7: Unit = Const
  return %7

fn Count(Int, Int) -> Int
bb0(n: Int, total: Int):
  %2: Int = Const 0
  %3: Bool = EqInt n, %2
  branch %3, bb1, bb2
bb1:
  return total
bb2:
  %4: Int = Const 1
  %5: Int = SubInt n, %4
  %6: Int = AddInt total, n
  tailcall Count, %5, %6
)");
}

TEST_CASE("Lowering gives every top-level definition its global before it lowers any function", "[midori_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Forward
def Total = fn() -> Int => later + 1;
def later: Int = 41;
Total();
)");

	REQUIRE(lowered.m_module.m_globals.size() == 2u);
	CHECK(lowered.m_module.m_globals[1u].m_name == "later");
	const MidoriIRFunction& total = lowered.m_module.m_functions[1u];
	REQUIRE(total.m_name == "Total");
	CHECK(MidoriIRPrinter(lowered.m_module).PrintFunction(total) == R"(fn Total() -> Int
bb0:
  later: Int = GlobalGet @1  !read
  %1: Int = Const 1
  %2: Int = AddInt later, %1
  return %2
)");
}

TEST_CASE("Lowering joins the branches of an if, a short-circuit and a Bool conversion in a block parameter", "[midori_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Joins
def Describe = fn(n: Int) -> Text => ((n > 0) && (n < 10)) as Text;
Describe(3);
)");

	CHECK(MidoriIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[1u]) == R"(fn Describe(Int) -> Text
bb0(n: Int):
  %1: Int = Const 0
  %2: Bool = GtInt n, %1
  branch %2, bb1, bb2(%2)
bb1:
  %4: Int = Const 10
  %5: Bool = LtInt n, %4
  jump bb2(%5)
bb2(%3: Bool):
  branch %3, bb3, bb4
bb3:
  %7: Text = Const "true"
  jump bb5(%7)
bb4:
  %8: Text = Const "false"
  jump bb5(%8)
bb5(%6: Text):
  return %6
)");
}

TEST_CASE("Lowering calls a builtin foreign function by name", "[midori_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Foreign
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
Print("hi");
)");

	CHECK(MidoriIRPrinter(lowered.m_module).Print() == R"(module Foreign
global @0 Print: Text
top-level $main$

fn $main$() -> Unit
bb0:
  %0: Text = Const "MIDORI_FFI_Print"
  %1: Unit = GlobalDefine @0, %0  !io
  %2: Text = Const "hi"
  %3: Unit = CallForeign foreign "MIDORI_FFI_Print", %2  !io
  %4: Unit = Const
  return %4
)");
}

TEST_CASE("A closure captures exactly the values its body reads", "[midori_ir][lowering][closure]")
{
	const LoweredModule lowered = RequireLowered(R"(module Capture
def Adder = fn(n: Int, unused: Int) -> fn(Int) -> Int => fn(m: Int) -> Int => n + m;
Adder(1, 2)(3);
)");

	const MidoriIRModule& module = lowered.m_module;
	CHECK(MidoriIRPrinter(module).PrintFunction(module.m_functions[1u]) == R"(fn Adder(Int, Int) -> fn(Int) -> Int
bb0(n: Int, unused: Int):
  %2: fn(Int) -> Int = MakeClosure Anonymous Function at line: 2, n  !alloc
  return %2
)");
	CHECK(MidoriIRPrinter(module).PrintFunction(module.m_functions[2u]) == R"(fn Anonymous Function at line: 2(Int) -> Int captures(Int)
bb0(m: Int):
  %1: Int = GetCapture #0
  %2: Int = AddInt %1, m
  return %2
)");
}

TEST_CASE("A local function that names itself fills its own capture once it is defined", "[midori_ir][lowering][closure]")
{
	const LoweredModule lowered = RequireLowered(R"(module Recursive
def Run = fn(limit: Int) -> Int => {
	def Loop = fn(n: Int) -> Int => if n >= limit then n else Loop(n + 1);
	Loop(0)
};
Run(3);
)");

	const std::string run = MidoriIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[1u]);
	CHECK(run.contains("= MakeClosure Anonymous Function at line: 3, limit  !alloc"));
	CHECK(run.contains("= BindCaptures #1, %"));
	CHECK(lowered.m_module.m_functions[2u].m_capture_types.size() == 2u);
}

TEST_CASE("Nothing is lowered to run after a call that returns Never", "[midori_ir][lowering][never]")
{
	const LoweredModule lowered = RequireLowered(R"(module Stops
foreign "MIDORI_FFI_Exit" ExitRaw: fn(Int) -> Unit;
def Stop = fn(code: Int) -> Never => Stop(code);
def Check = fn(n: Int) -> Int => {
	def checked = if n < 0 then Stop(1) else n;
	checked + 1
};
Check(1);
)");

	CHECK(MidoriIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[2u]) == R"(fn Check(Int) -> Int
bb0(n: Int):
  %1: Int = Const 0
  %2: Bool = LtInt n, %1
  branch %2, bb2, bb3
bb1(%5: Int):
  %6: Int = Const 1
  %7: Int = AddInt %5, %6
  return %7
bb2:
  %3: Int = Const 1
  %4: Never = Call Stop, %3  !call
  unreachable
bb3:
  jump bb1(n)
bb4:
  jump bb1(%4)
)");
}

TEST_CASE("Every prelude module lowers to MidoriIR that verifies", "[midori_ir][lowering][prelude]")
{
	const std::filesystem::path prelude(MARMOT_PRELUDE_DIR);
	std::vector<std::filesystem::path> modules;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(prelude))
	{
		if (entry.path().extension() == ".mmt")
		{
			modules.push_back(entry.path());
		}
	}
	REQUIRE(modules.size() > 10u);

	const CompilerTestMode::ScopedOverride test_mode_override(true);
	for (const std::filesystem::path& module : modules)
	{
		INFO(module.string());
		MidoriDriver::CompileFileWithReportResult compiled = MidoriDriver::CompileFileWithReport(module, MidoriDriver::EnvironmentCompilationInputs());
		if (!compiled.has_value())
		{
			FAIL(compiled.error().Rendered());
		}
	}
}

TEST_CASE("Lowering reports an unknown builtin foreign function", "[midori_ir][lowering][diagnostics][ffi]")
{
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Lowering;
	expectation.m_code = CompilerErrorCode::LoweringUnknownForeignFunction;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Unknown foreign function 'MIDORI_FFI_PrintLin'" };
	RequireErrorMatches(RequireLoweringError(R"(module ForeignTypo
foreign "MIDORI_FFI_PrintLin" PrintTypo : fn(Text) -> Unit;
)"), expectation);
}

TEST_CASE("A program of scalars, branches and tail calls prints what it computes", "[midori_ir][lowering][backend]")
{
	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> executed = MidoriTest::ExecuteSnippet(R"(module Parity
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
def Fib = fn(n: Int, a: Int, b: Int) -> Int => if n == 0 then a else Fib(n - 1, b, a + b);
def Show = fn(label: Text, value: Int) -> Unit => Print(label ++ "=" ++ (value as Text) ++ "\n");
def Pick = fn(flag: Bool) -> Text => if flag then "yes" else "no";
Show("fib", Fib(50, 0, 1));
Show("mixed", {
    def x = Fib(10, 0, 1);
    def y = if (x > 50) || (x < 0) then x * 2 else x / 2;
    y - (x % 7)
});
Print(Pick((Fib(5, 0, 1) == 5) && !(Fib(6, 0, 1) == 5)) ++ "\n");
Print(((Fib(7, 0, 1) as Float) / 2.0) as Text);
)");
	REQUIRE(executed.has_value());
	CHECK(executed->m_exit_code == 0);
	CHECK(executed->m_output.m_stdout == "fib=12586269025\nmixed=104\nyes\n6.5");
}

TEST_CASE("Lowering reads and calls another module's globals through imports", "[midori_ir][lowering][module]")
{
	const MidoriTest::TempProject project(
	{
		MidoriTest::TempProjectFile("Lib.mmt", R"(module Lib
public export { Scale, factor }
def factor = 3;
def Scale = fn(n: Int) -> Int => n * factor;
)"),
		MidoriTest::TempProjectFile("Main.mmt", R"(module Main
import { "./Lib.mmt" }
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
Print((Lib::Scale(Lib::factor) as Text) ++ "\n");
)")
	});

	const CompilerTestMode::ScopedOverride test_mode_override(true);
	MidoriDriver::CompileFileWithReportResult compiled = MidoriDriver::CompileFileWithReport(project.Path("Main.mmt"), MidoriDriver::EnvironmentCompilationInputs().WithEmitMidoriIR(true));
	if (!compiled.has_value())
	{
		FAIL(compiled.error().Rendered());
	}

	const std::vector<std::string>& modules = compiled->m_midori_ir;
	REQUIRE(modules.size() == 2u);
	CHECK(modules[1u].contains("global @1 Lib::factor: Int\nglobal @2 Lib::Scale: fn(Int) -> Int\n"));
	CHECK(modules[1u].contains("%3: Int = CallGlobal @2, factor  !call"));
}
