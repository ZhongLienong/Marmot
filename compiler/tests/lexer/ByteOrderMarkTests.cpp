#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Common/Source/Source.h"
#include "Utility/Formatter/Formatter.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"

#include <expected>
#include <fstream>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace
{
	// What an editor on Windows writes at the start of a UTF-8 file.
	std::string WithByteOrderMark(std::string_view source_code)
	{
		return std::string(MidoriSource::UTF8_BYTE_ORDER_MARK) + std::string(source_code);
	}

	std::string ReadFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	}
}

TEST_CASE("A byte order mark is not part of the program", "[lexer][source]")
{
	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> executed = MidoriTest::ExecuteSnippet(
		WithByteOrderMark("module Main\ndef main = fn() -> Int => 0;\n"));

	REQUIRE(executed.has_value());
	CHECK(executed->m_exit_code == 0);
}

TEST_CASE("A diagnostic from a file with a byte order mark quotes the line without it", "[lexer][source]")
{
	const MidoriResult::CompilerResult compiled = MidoriTest::CompileSnippet(
		WithByteOrderMark("module Main\ndef broken = ;\n"));

	REQUIRE_FALSE(compiled.has_value());
	const std::string rendered = MidoriTest::StripAnsiCodes(compiled.error().Rendered());
	CHECK_THAT(rendered, ContainsSubstring("def broken = ;"));
	CHECK(rendered.find(MidoriSource::UTF8_BYTE_ORDER_MARK) == std::string::npos);
}

TEST_CASE("Formatting keeps the byte order mark the file came with", "[formatter][source]")
{
	const MidoriTest::TempProject project({ MidoriTest::TempProjectFile(
		"Main.mmt",
		WithByteOrderMark("module Main\ndef   main = fn() -> Int => 0;\n")) });
	const std::filesystem::path path = project.Path("Main.mmt");

	const MidoriFormatter::RunResult written = MidoriFormatter::FormatPath(path, MidoriFormatter::Options{ true, false });
	REQUIRE(written.m_files.size() == 1u);
	REQUIRE_FALSE(written.m_files.front().m_error.has_value());
	CHECK(written.m_files.front().m_changed);

	const std::string formatted = ReadFile(path);
	CHECK(MidoriSource::StartsWithByteOrderMark(formatted));
	CHECK_THAT(formatted, ContainsSubstring("def main = fn() -> Int => 0;"));

	// Formatting it again changes nothing: the mark is not a difference.
	const MidoriFormatter::RunResult checked = MidoriFormatter::FormatPath(path, MidoriFormatter::Options{ false, true });
	REQUIRE(checked.m_files.size() == 1u);
	CHECK_FALSE(checked.m_files.front().m_changed);
}
