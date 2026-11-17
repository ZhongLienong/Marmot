#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"

#include <expected>
#include <string>

namespace
{
	void RequireWarningMatches(const std::vector<CompilerWarning>& warnings, CompilerWarningCode code, const MarmotTest::WarningExpectation& expectation)
	{
		const CompilerWarning* warning = MarmotTest::FindWarning(warnings, code);
		REQUIRE(warning != nullptr);

		std::string mismatch;
		const bool matched = MarmotTest::Matches(*warning, expectation, &mismatch);
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

	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyze_result = MarmotTest::AnalyzeSnippet(source_code, "AnalyzerUnused.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::UnusedLocal,
		MarmotTest::WarningExpectation
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

	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyze_result = MarmotTest::AnalyzeSnippet(source_code, "AnalyzerShadowing.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::NameShadowing,
		MarmotTest::WarningExpectation
		{
			.m_stage = CompilerStage::StaticAnalyzer,
			.m_code = CompilerWarningCode::NameShadowing,
			.m_line = 4,
			.m_message_substrings = { "Name 'value' shadows a variable from an outer scope." },
			.m_rendered_substrings = { "AnalyzerShadowing.mmt:4", "def value = 2;" }
		});
}

TEST_CASE("StaticAnalyzer does not warn when a closure is returned", "[static-analyzer]")
{
	// Bindings are immutable and a closure keeps what it captured alive, so a
	// closure outliving its scope is how currying and counters work, not a mistake.
	const std::string source_code =
		R"(module AnalyzerCapture
def MakeCounter = fn() -> fn() -> Int => {
	def value = 1;
	def next = fn() -> Int => value;
	next
};
def Add = fn(a: Int) -> fn(Int) -> Int => fn(b: Int) -> Int => a + b;
)";

	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyze_result = MarmotTest::AnalyzeSnippet(source_code, "AnalyzerCapture.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	CHECK(analyze_result->m_warnings.empty());
}

TEST_CASE("StaticAnalyzer keeps sibling scopes independent and reads enclosing bindings", "[static-analyzer]")
{
	const std::string source = R"(module AnalyzerScopeOwnership
def Compute = fn(value: Int) -> Int => {
	def first = {
		def value = 2;
		def unused = 3;
		value
	};
	def second = {
		def unused = 4;
		value
	};
	first + second
};
)";
	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyzed = MarmotTest::AnalyzeSnippet(source, "AnalyzerScopeOwnership.mmt");
	if (!analyzed.has_value())
	{
		FAIL(analyzed.error().Rendered());
	}

	std::vector<int> unused_lines;
	std::vector<int> shadow_lines;
	for (const CompilerWarning& warning : analyzed->m_warnings)
	{
		REQUIRE(warning.m_location.has_value());
		if (warning.m_code == CompilerWarningCode::UnusedLocal)
		{
			CHECK(warning.m_message == "Binding 'unused' is never read.");
			unused_lines.push_back(warning.m_location->m_line);
		}
		if (warning.m_code == CompilerWarningCode::NameShadowing)
		{
			CHECK(warning.m_message == "Name 'value' shadows a variable from an outer scope.");
			shadow_lines.push_back(warning.m_location->m_line);
		}
	}
	CHECK(unused_lines == std::vector<int>{ 5, 9 });
	CHECK(shadow_lines == std::vector<int>{ 4 });
}

TEST_CASE("StaticAnalyzer warns on literal integer overflow patterns", "[static-analyzer]")
{
	const std::string source_code =
		R"(module AnalyzerOverflow
def too_big = 9223372036854775807 + 1;
def shifted = 1 << 63;
def main = fn() -> Int => too_big + shifted;
)";

	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyze_result = MarmotTest::AnalyzeSnippet(source_code, "AnalyzerOverflow.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	RequireWarningMatches(
		analyze_result->m_warnings,
		CompilerWarningCode::IntegerOverflow,
		MarmotTest::WarningExpectation
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

	std::expected<MarmotTest::AnalyzedSnippet, CompilerError> analyze_result = MarmotTest::AnalyzeSnippet(source_code, "AnalyzerOverflowSafe.mmt");
	if (!analyze_result.has_value())
	{
		FAIL(std::string(analyze_result.error().Rendered()));
	}

	CHECK(MarmotTest::FindWarning(analyze_result->m_warnings, CompilerWarningCode::IntegerOverflow) == nullptr);
}
