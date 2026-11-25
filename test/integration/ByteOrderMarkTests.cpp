#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Compiler/Source/Source.h"
#include "Utility/Formatter/Formatter.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"
#include "Support/Expected.h"

#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace
{
	// What an editor on Windows writes at the start of a UTF-8 file.
	std::string WithByteOrderMark(std::string_view source_code)
	{
		return std::string(MarmotSource::UTF8_BYTE_ORDER_MARK) + std::string(source_code);
	}

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	}
}

TEST_CASE("A byte order mark is not part of the program", "[lexer][source]")
{
	const MarmotCompiler::Expected<MarmotTest::ExecutedSnippet, CompilerError> executed = MarmotTest::ExecuteSnippet(
		WithByteOrderMark("module Main\ndef main = fn() -> Int => 0;\n"));

	REQUIRE(executed.HasValue());
	CHECK(executed->m_exit_code == 0);
}
