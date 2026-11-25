#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Utility/Driver/MarmotDriver.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/OutputCapture.h"
#include "support/TempProject.h"

namespace
{
	void RequireWarningMatches(const CompilerWarning& warning, const MarmotTest::WarningExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MarmotTest::Matches(warning, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}

	std::string RenderedFailure(const std::filesystem::path& main_file_path, size_t jobs)
	{
		const MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(main_file_path, MarmotDriver::EnvironmentCompilationInputs().WithJobs(jobs));
		REQUIRE_FALSE(compiled.HasValue());
		return MarmotTest::StripAnsiCodes(compiled.Error().Rendered());
	}
}

TEST_CASE("CompileFileWithReport preserves aggregated warning order without rendering warnings early", "[compiler][warning][report]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile
		(
			"Alpha.mmt",
			R"(module Alpha
public export { Value }
def Value = fn() -> Int => {
	def used = 1;
	def unusedAlpha = 2;
	used
};
)"
		),
		MarmotTest::TempProjectFile
		(
			"Zulu.mmt",
			R"(module Zulu
public export { Value }
def Value = fn() -> Int => {
	def used = 3;
	def unusedZulu = 4;
	used
};
)"
		),
		MarmotTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "Alpha.mmt", "Zulu.mmt" }
def main = fn() -> Int => Alpha::Value() + Zulu::Value();
)"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));

	MarmotTest::OutputCapture capture;
	MarmotDriver::CompileFileWithReportResult compile_result = MarmotDriver::CompileFileWithReport(main_file_path);
	const MarmotTest::CapturedOutput output = capture.Stop();
	if (!compile_result.HasValue())
	{
		FAIL(MarmotTest::StripAnsiCodes(compile_result.Error().Rendered()));
	}

	const MarmotResult::CompilerReport& report = compile_result->Report();
	REQUIRE(report.m_warnings.Size() == 2u);
	REQUIRE(report.m_errors.Empty());
	REQUIRE(report.m_warnings.m_items[0u].m_location.HasValue());
	REQUIRE(report.m_warnings.m_items[1u].m_location.HasValue());
	CHECK(std::filesystem::path(report.m_warnings.m_items[0u].m_location->m_file_name).filename().string() == "Alpha.mmt");
	CHECK(std::filesystem::path(report.m_warnings.m_items[1u].m_location->m_file_name).filename().string() == "Zulu.mmt");

	const std::string rendered_output = MarmotTest::StripAnsiCodes(output.m_stdout);
	CHECK(rendered_output.find("1 warning(s) in Alpha.mmt") == std::string::npos);
	CHECK(rendered_output.find("1 warning(s) in Zulu.mmt") == std::string::npos);
	CHECK(rendered_output.find("Static Analyzer Warning at") == std::string::npos);
}

TEST_CASE("Compiler report preserves warnings when a later compilation stage fails", "[compiler][warning][report]")
{
	const std::string source_code =
		R"(module WarningWithCodegenFailure
type Pair =
{
	value: Int
};
foreign "MARMOT_FFI_ReadPairA" ReadPairA : fn() -> Pair;
foreign "MARMOT_FFI_ReadPairB" ReadPairB : fn() -> Pair;
def main = fn() -> Int => {
	def used = 1;
	def unused = 2;
	ReadPairA();
	ReadPairB();
	used
};
)";

	MarmotResult::CompiledProgramReportResult compile_result =
		MarmotTest::CompileSnippetWithReport(source_code, "WarningWithCodegenFailure.mmt");
	REQUIRE_FALSE(compile_result.HasValue());

	const MarmotResult::CompilerReport& report = MarmotTest::CompilationReport(compile_result);
	REQUIRE_FALSE(report.m_warnings.Empty());
	REQUIRE_FALSE(report.m_errors.Empty());
	REQUIRE(report.m_warnings.Size() == 1u);
	REQUIRE(report.m_errors.Size() == 2u);

	const CompilerWarning* warning =
		MarmotTest::FindWarning(report, CompilerStage::StaticAnalyzer, CompilerWarningCode::UnusedLocal);
	REQUIRE(warning != nullptr);
	RequireWarningMatches(
		*warning,
		MarmotTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::UnusedLocal,
			.m_line = 10
		});

	REQUIRE(MarmotTest::FindError(report.m_errors, CompilerStage::Lowering, CompilerErrorCode::LoweringUnsupportedConstruct) != nullptr);
}

TEST_CASE("A successful compile renders its warnings from the final report", "[compiler][warning][driver]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
def main = fn() -> Int => {
	def used = 1;
	def unused = 2;
	used
};
)"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));

	const MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(main_file_path);
	REQUIRE(compiled.HasValue());

	const std::string rendered_output = MarmotTest::StripAnsiCodes(compiled->Report().RenderedWarnings());
	const size_t summary_position = rendered_output.find("[warning] 1 warning(s) in Main.mmt");
	const size_t warning_position = rendered_output.find("Static Analyzer Warning at");
	REQUIRE(summary_position != std::string::npos);
	REQUIRE(warning_position != std::string::npos);
	CHECK(summary_position < warning_position);
	CHECK(rendered_output.find("[warning] 1 warning(s) in Main.mmt", summary_position + 1u) == std::string::npos);
}

TEST_CASE("Every failing module is reported in build order for any worker count", "[compiler][report]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile
		(
			"Alpha.mmt",
			R"(module Alpha
public export { Value }
def Value: Int = "alpha";
)"
		),
		MarmotTest::TempProjectFile
		(
			"Zulu.mmt",
			R"(module Zulu
public export { Value }
def Value: Int = "zulu";
)"
		),
		MarmotTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "Alpha.mmt", "Zulu.mmt" }
def total = Alpha::Value + Zulu::Value;
)"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::string sequential = RenderedFailure(main_file_path, 1u);
	const size_t alpha = sequential.find("Alpha.mmt:3");
	const size_t zulu = sequential.find("Zulu.mmt:3");
	REQUIRE(alpha != std::string::npos);
	REQUIRE(zulu != std::string::npos);
	CHECK(alpha < zulu);
	CHECK(sequential.find("Main.mmt") == std::string::npos);

	for (size_t run = 0u; run < 20u; run += 1u)
	{
		CHECK(RenderedFailure(main_file_path, 4u) == sequential);
	}
}
