#include <cstdlib>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"

TEST_CASE("MidoriCore links into the unit test target", "[smoke]")
{
	std::string source_code =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result = MidoriTest::ExecuteSnippet(std::move(source_code), "Smoke.mmt");
	if (!run_result.has_value())
	{
		FAIL(std::string(run_result.error().Rendered()));
	}

	REQUIRE(run_result.value().m_exit_code == EXIT_SUCCESS);
}
