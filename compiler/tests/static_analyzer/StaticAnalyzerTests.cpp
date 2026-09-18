#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"

#include <expected>
#include <string>

namespace
{
	void RequireWarningMatches(const std::vector<CompilerWarning>& warnings, CompilerWarningCode code, const MidoriTest::WarningExpectation& expectation)
	{
		const CompilerWarning* warning = MidoriTest::FindWarning(warnings, code);
		REQUIRE(warning != nullptr);

		std::string mismatch;
		const bool matched = MidoriTest::Matches(*warning, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("StaticAnalyzer reports unread locals with source context", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerUnused
def Compute = fn() -> Int => {
	def used = 1;
	def unused = 2;
	used
};
)";

	std::expected<MidoriTest::AnalyzedSnippet, CompilerError> analyze_result = MidoriTest::AnalyzeSnippet(source_code, "AnalyzerUnused.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	REQUIRE(analyze_result->m_errors.empty());
	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::UnusedLocal,
		MidoriTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::UnusedLocal,
			.m_line = 4,
			.m_message_substrings = { "Binding 'unused' is never read." },
			.m_rendered_substrings = { "AnalyzerUnused.mmt:4", "def unused = 2;" }
		});
}

TEST_CASE("StaticAnalyzer warns when an inner scope shadows an outer binding", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerShadowing
def Compute = fn(value : Int) -> Int => {
	{
		def value = 2;
		value
	};
	value
};
)";

	std::expected<MidoriTest::AnalyzedSnippet, CompilerError> analyze_result = MidoriTest::AnalyzeSnippet(source_code, "AnalyzerShadowing.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	REQUIRE(analyze_result->m_errors.empty());
	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::NameShadowing,
		MidoriTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::NameShadowing,
			.m_line = 4,
			.m_message_substrings = { "Name 'value' shadows a variable from an outer scope." },
			.m_rendered_substrings = { "AnalyzerShadowing.mmt:4", "def value = 2;" }
		});
}

TEST_CASE("StaticAnalyzer reports captured closures that escape as return values", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerCapture
def MakeCounter = fn() -> fn() -> Int => {
	def value = 1;
	def next = fn() -> Int => value;
	next
};
)";

	std::expected<MidoriTest::AnalyzedSnippet, CompilerError> analyze_result = MidoriTest::AnalyzeSnippet(source_code, "AnalyzerCapture.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	REQUIRE(analyze_result->m_errors.empty());
	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::CaptureEscape,
		MidoriTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::CaptureEscape,
			.m_line = 5,
			.m_message_substrings = { "Captured closure 'next' escapes its defining scope as a return value." },
			.m_rendered_substrings = { "AnalyzerCapture.mmt:5", "next" }
		});
}

TEST_CASE("StaticAnalyzer warns on literal integer overflow patterns", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerOverflow
def too_big = 9223372036854775807 + 1;
def shifted = 1 << 63;
def main = fn() -> Int => too_big + shifted;
)";

	std::expected<MidoriTest::AnalyzedSnippet, CompilerError> analyze_result = MidoriTest::AnalyzeSnippet(source_code, "AnalyzerOverflow.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	REQUIRE(analyze_result->m_errors.empty());
	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::IntegerOverflow,
		MidoriTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::IntegerOverflow,
			.m_line = 2,
			.m_message_substrings = { "overflow" },
			.m_rendered_substrings = { "AnalyzerOverflow.mmt:2", "9223372036854775807 + 1" }
		});
}

TEST_CASE("StaticAnalyzer does not warn on non-literal integer arithmetic", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerOverflowSafe
def Compute = fn(x : Int) -> Int => {
	x + 1
};
)";

	std::expected<MidoriTest::AnalyzedSnippet, CompilerError> analyze_result = MidoriTest::AnalyzeSnippet(source_code, "AnalyzerOverflowSafe.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	REQUIRE(analyze_result->m_errors.empty());
	CHECK(MidoriTest::FindWarning(analyze_result->m_warnings, CompilerWarningCode::IntegerOverflow) == nullptr);
}
