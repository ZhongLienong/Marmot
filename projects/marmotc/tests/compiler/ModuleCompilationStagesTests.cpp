#include <catch2/catch_test_macros.hpp>

#include "Utility/Driver/MarmotDriver.h"
#include "support/TempProject.h"

TEST_CASE("Backend failure keeps warnings and does not prevent checking a published module's importer", "[compiler][module-stages]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Dependency.mmt", R"(module Dependency
public export { Value }
foreign "TooManyArguments" Native: fn(Int, Int, Int, Int, Int) -> Int from "never_loaded";
def Value = 1;
def Work = fn() -> Int => {
    def unused = 0;
    Native(1, 2, 3, 4, 5)
};
Work();
)"),
		MarmotTest::TempProjectFile("Main.mmt", R"(module Main
import { "Dependency.mmt" }
def wrong: Bool = Dependency::Value;
)")
	});
	std::string expected;
	for (size_t jobs : { 1u, 2u, 4u })
	{
		CAPTURE(jobs);
		const MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(project.Path("Main.mmt"), CompilationInputs().WithJobs(jobs));
		REQUIRE_FALSE(compiled.HasValue());
		const MarmotResult::CompilerReport& report = compiled.Error().m_report;
		INFO(report.Rendered());
		REQUIRE(report.m_errors.Size() == 2u);
		CHECK(report.m_errors.m_items[0u].m_code == CompilerErrorCode::CodeGeneratorLimitExceeded);
		CHECK(report.m_errors.m_items[1u].m_code == CompilerErrorCode::TypeMismatch);
		REQUIRE(report.m_warnings.Size() == 1u);
		CHECK(report.m_warnings.First().m_code == CompilerWarningCode::UnusedLocal);
		if (expected.empty())
		{
			expected = report.Rendered();
		}
		CHECK(report.Rendered() == expected);
	}
}

TEST_CASE("Invalid exports keep analysis warnings and block importer checking", "[compiler][module-stages]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Dependency.mmt", R"(module Dependency
public export { Missing }
def Work = fn() -> Int => {
    def unused = 0;
    1
};
)"),
		MarmotTest::TempProjectFile("Main.mmt", R"(module Main
import { "Dependency.mmt" }
def wrong: Bool = 1;
)")
	});
	for (size_t jobs : { 1u, 4u })
	{
		CAPTURE(jobs);
		const MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(project.Path("Main.mmt"), CompilationInputs().WithJobs(jobs));
		REQUIRE_FALSE(compiled.HasValue());
		const MarmotResult::CompilerReport& report = compiled.Error().m_report;
		INFO(report.Rendered());
		REQUIRE(report.m_errors.Size() == 1u);
		CHECK(report.m_errors.First().m_code == CompilerErrorCode::ModuleMissingExportedSymbol);
		REQUIRE(report.m_warnings.Size() == 1u);
		CHECK(report.m_warnings.First().m_code == CompilerWarningCode::UnusedLocal);
	}
}
