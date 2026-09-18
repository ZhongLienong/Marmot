#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/OutputCapture.h"
#include "support/TempProject.h"

namespace
{
	void RequireWarningMatches(const CompilerWarning& warning, const MidoriTest::WarningExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(warning, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("CompileFileWithReport preserves aggregated warning order without rendering warnings early", "[compiler][warning][report]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
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
		MidoriTest::TempProjectFile
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
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			R"(module Main
import { "Alpha.mmt", "Zulu.mmt" }
def main = fn() -> Int => Alpha::Value() + Zulu::Value();
)"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));

	MidoriTest::OutputCapture capture;
	MidoriDriver::CompileFileWithReportResult compile_result = MidoriDriver::CompileFileWithReport(main_file_path);
	const MidoriTest::CapturedOutput output = capture.Stop();
	if (!compile_result.has_value())
	{
		FAIL(MidoriTest::StripAnsiCodes(compile_result.error().Rendered()));
	}

	const MidoriResult::CompilerReport& report = compile_result->Report();
	REQUIRE(report.WarningCount() == 2u);
	REQUIRE_FALSE(report.HasErrors());
	REQUIRE(report.Warnings().Warnings()[0u].m_location.has_value());
	REQUIRE(report.Warnings().Warnings()[1u].m_location.has_value());
	CHECK(std::filesystem::path(report.Warnings().Warnings()[0u].m_location->m_file_name).filename().string() == "Alpha.mmt");
	CHECK(std::filesystem::path(report.Warnings().Warnings()[1u].m_location->m_file_name).filename().string() == "Zulu.mmt");

	const std::string rendered_output = MidoriTest::StripAnsiCodes(output.m_stdout);
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
foreign "MIDORI_FFI_ReadPairA" ReadPairA : fn() -> Pair;
foreign "MIDORI_FFI_ReadPairB" ReadPairB : fn() -> Pair;
def main = fn() -> Int => {
	def used = 1;
	def unused = 2;
	ReadPairA();
	ReadPairB();
	used
};
)";

	MidoriResult::CompilationResult compile_result =
		MidoriTest::CompileSnippetWithReport(source_code, "WarningWithCodegenFailure.mmt");
	REQUIRE_FALSE(compile_result.has_value());

	const MidoriResult::CompilerReport& report = MidoriTest::CompilationReport(compile_result);
	REQUIRE(report.HasWarnings());
	REQUIRE(report.HasErrors());
	REQUIRE(report.WarningCount() == 1u);
	REQUIRE(report.ErrorCount() == 2u);

	const CompilerWarning* warning =
		MidoriTest::FindWarning(report, CompilerStage::StaticAnalyzer, CompilerWarningCode::UnusedLocal);
	REQUIRE(warning != nullptr);
	RequireWarningMatches(
		*warning,
		MidoriTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::UnusedLocal,
			.m_line = 10
		});

	REQUIRE(MidoriTest::FindError(report.Errors(), CompilerStage::CodeGenerator, CompilerErrorCode::CodeGeneratorUnsupportedLowering) != nullptr);
}

TEST_CASE("CompileAndRunFile renders successful warnings from the final report", "[compiler][warning][driver]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
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

	MidoriTest::OutputCapture capture;
	MidoriDriver::DriverResult run_result = MidoriDriver::CompileAndRunFile(main_file_path);
	const MidoriTest::CapturedOutput output = capture.Stop();
	REQUIRE(run_result.has_value());

	const std::string rendered_output = MidoriTest::StripAnsiCodes(output.m_stdout);
	const size_t summary_position = rendered_output.find("[warning] 1 warning(s) in Main.mmt");
	const size_t warning_position = rendered_output.find("Static Analyzer Warning at");
	REQUIRE(summary_position != std::string::npos);
	REQUIRE(warning_position != std::string::npos);
	CHECK(summary_position < warning_position);
	CHECK(rendered_output.find("[warning] 1 warning(s) in Main.mmt", summary_position + 1u) == std::string::npos);
}
