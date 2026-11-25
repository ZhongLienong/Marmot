#include <cstdlib>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"
#include "Support/Expected.h"

TEST_CASE("The Marmot libraries link into the unit test target", "[smoke]")
{
	std::string source_code =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	MarmotCompiler::Expected<MarmotTest::ExecutedSnippet, CompilerError> run_result = MarmotTest::ExecuteSnippet(std::move(source_code), "Smoke.mmt");
	if (!run_result.HasValue())
	{
		FAIL(std::string(run_result.Error().Rendered()));
	}

	REQUIRE(run_result.Value().m_exit_code == EXIT_SUCCESS);
}
