#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MarmotIR/Printer/MarmotIRPrinter.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "Utility/Driver/MarmotDriver.h"
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
		std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> lowered = MarmotTest::LowerSnippetWithDiagnostics(std::move(source));
		if (!lowered.has_value())
		{
			FAIL(lowered.error().Rendered());
		}
		const std::vector<MarmotIRViolation> violations = MarmotIRVerifier(lowered->m_module).Verify();
		for (const MarmotIRViolation& violation : violations)
		{
			UNSCOPED_INFO(violation.ToString());
		}
		REQUIRE(violations.empty());
		return std::move(lowered).value();
	}

	CompilerError RequireLoweringError(std::string source)
	{
		std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> lowered = MarmotTest::LowerSnippetWithDiagnostics(std::move(source));
		REQUIRE_FALSE(lowered.has_value());
		REQUIRE(lowered.error().Size() == 1u);
		return lowered.error().m_items.front();
	}

	void RequireErrorMatches(const CompilerError& error, const MarmotTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MarmotTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("Lowering names a top-level function directly and turns a call in tail position into a tail call", "[marmot_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Countdown
def Count = fn(n: Int, total: Int) -> Int => if n == 0 then total else Count(n - 1, total + n);
def start = 3;
Count(start, 0);
)");

	CHECK(MarmotIRPrinter(lowered.m_module).Print() == R"(module Countdown
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

TEST_CASE("Lowering gives every top-level definition its global before it lowers any function", "[marmot_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Forward
def Total = fn() -> Int => later + 1;
def later: Int = 41;
Total();
)");

	REQUIRE(lowered.m_module.m_globals.size() == 2u);
	CHECK(lowered.m_module.m_globals[1u].m_name == "later");
	const MarmotIRFunction& total = lowered.m_module.m_functions[1u];
	REQUIRE(total.m_name == "Total");
	CHECK(MarmotIRPrinter(lowered.m_module).PrintFunction(total) == R"(fn Total() -> Int
bb0:
  later: Int = GlobalGet @1  !read
  %1: Int = Const 1
  %2: Int = AddInt later, %1
  return %2
)");
}

TEST_CASE("Lowering joins the branches of an if, a short-circuit and a Bool conversion in a block parameter", "[marmot_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Joins
def Describe = fn(n: Int) -> Text => ((n > 0) && (n < 10)) as Text;
Describe(3);
)");

	CHECK(MarmotIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[1u]) == R"(fn Describe(Int) -> Text
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

TEST_CASE("Lowering calls a builtin foreign function by name", "[marmot_ir][lowering]")
{
	const LoweredModule lowered = RequireLowered(R"(module Foreign
foreign "MARMOT_FFI_ReportPanic" Report: fn(Text) -> Unit;
Report("hi");
)");

	CHECK(MarmotIRPrinter(lowered.m_module).Print() == R"(module Foreign
global @0 Report: Text
top-level $main$

fn $main$() -> Unit
bb0:
  %0: Text = Const "MARMOT_FFI_ReportPanic"
  %1: Unit = GlobalDefine @0, %0  !io
  %2: Text = Const "hi"
  %3: Unit = CallForeign foreign "MARMOT_FFI_ReportPanic", %2  !io
  %4: Unit = Const
  return %4
)");
}

TEST_CASE("A closure captures exactly the values its body reads", "[marmot_ir][lowering][closure]")
{
	const LoweredModule lowered = RequireLowered(R"(module Capture
def Adder = fn(n: Int, unused: Int) -> fn(Int) -> Int => fn(m: Int) -> Int => n + m;
Adder(1, 2)(3);
)");

	const MarmotIRModule& module = lowered.m_module;
	CHECK(MarmotIRPrinter(module).PrintFunction(module.m_functions[1u]) == R"(fn Adder(Int, Int) -> fn(Int) -> Int
bb0(n: Int, unused: Int):
  %2: fn(Int) -> Int = MakeClosure Anonymous Function at line: 2, n  !alloc
  return %2
)");
	CHECK(MarmotIRPrinter(module).PrintFunction(module.m_functions[2u]) == R"(fn Anonymous Function at line: 2(Int) -> Int captures(Int)
bb0(m: Int):
  %1: Int = GetCapture #0
  %2: Int = AddInt %1, m
  return %2
)");
}

TEST_CASE("A local function that names itself fills its own capture once it is defined", "[marmot_ir][lowering][closure]")
{
	const LoweredModule lowered = RequireLowered(R"(module Recursive
def Run = fn(limit: Int) -> Int => {
	def Loop = fn(n: Int) -> Int => if n >= limit then n else Loop(n + 1);
	Loop(0)
};
Run(3);
)");

	const std::string run = MarmotIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[1u]);
	CHECK(run.contains("= MakeClosure Anonymous Function at line: 3, limit  !alloc"));
	CHECK(run.contains("= BindCaptures #1, %"));
	CHECK(lowered.m_module.m_functions[2u].m_capture_types.size() == 2u);
}

TEST_CASE("Nothing is lowered to run after a call that returns Never", "[marmot_ir][lowering][never]")
{
	const LoweredModule lowered = RequireLowered(R"(module Stops
foreign "MARMOT_FFI_Exit" ExitRaw: fn(Int) -> Unit;
def Stop = fn(code: Int) -> Never => Stop(code);
def Check = fn(n: Int) -> Int => {
	def checked = if n < 0 then Stop(1) else n;
	checked + 1
};
Check(1);
)");

	CHECK(MarmotIRPrinter(lowered.m_module).PrintFunction(lowered.m_module.m_functions[2u]) == R"(fn Check(Int) -> Int
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

TEST_CASE("Every prelude module lowers to MarmotIR that verifies", "[marmot_ir][lowering][prelude]")
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

	for (const std::filesystem::path& module : modules)
	{
		INFO(module.string());
		MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(module, MarmotDriver::EnvironmentCompilationInputs());
		if (!compiled.has_value())
		{
			FAIL(compiled.error().Rendered());
		}
	}
}

TEST_CASE("Lowering reports an unknown builtin foreign function", "[marmot_ir][lowering][diagnostics][ffi]")
{
	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Lowering;
	expectation.m_code = CompilerErrorCode::LoweringUnknownForeignFunction;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Unknown foreign function 'MARMOT_FFI_PrintLin'" };
	RequireErrorMatches(RequireLoweringError(R"(module ForeignTypo
foreign "MARMOT_FFI_PrintLin" PrintTypo : fn(Text) -> Unit;
)"), expectation);
}

TEST_CASE("Lowering reads and calls another module's globals through imports", "[marmot_ir][lowering][module]")
{
	const MarmotTest::TempProject project(
	{
		MarmotTest::TempProjectFile("Lib.mmt", R"(module Lib
public export { Scale, factor }
def factor = 3;
def Scale = fn(n: Int) -> Int => n * factor;
)"),
		MarmotTest::TempProjectFile("Main.mmt", R"(module Main
import { "./Lib.mmt" }
foreign "MARMOT_FFI_Print" Print: fn(Text) -> Unit from "marmotstdlib";
Print((Lib::Scale(Lib::factor) as Text) ++ "\n");
)")
	});

	MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(project.Path("Main.mmt"), MarmotDriver::EnvironmentCompilationInputs().WithEmitMarmotIR(true));
	if (!compiled.has_value())
	{
		FAIL(compiled.error().Rendered());
	}

	const std::vector<std::string>& modules = compiled->m_marmot_ir;
	REQUIRE(modules.size() == 2u);
	CHECK(modules[1u].contains("global @1 Lib::factor: Int\nglobal @2 Lib::Scale: fn(Int) -> Int\n"));
	CHECK(modules[1u].contains("%3: Int = CallGlobal @2, factor  !call"));
}
