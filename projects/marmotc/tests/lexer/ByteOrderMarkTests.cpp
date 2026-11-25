#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Compiler/Source/Source.h"
#include "Utility/Formatter/Formatter.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"

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

TEST_CASE("A diagnostic from a file with a byte order mark quotes the line without it", "[lexer][source]")
{
	const MarmotResult::ExecutableDiagnosticsResult compiled = MarmotTest::CompileSnippet(
		WithByteOrderMark("module Main\ndef broken = ;\n"));

	REQUIRE_FALSE(compiled.HasValue());
	const std::string rendered = MarmotTest::StripAnsiCodes(compiled.Error().Rendered());
	CHECK_THAT(rendered, ContainsSubstring("def broken = ;"));
	CHECK(rendered.find(MarmotSource::UTF8_BYTE_ORDER_MARK) == std::string::npos);
}

TEST_CASE("Formatting keeps the byte order mark the file came with", "[formatter][source]")
{
	const MarmotTest::TempProject project({ MarmotTest::TempProjectFile(
		"Main.mmt",
		WithByteOrderMark("module Main\ndef   main = fn() -> Int => 0;\n")) });
	const std::filesystem::path path = project.Path("Main.mmt");

	const MarmotFormatter::RunResult written = MarmotFormatter::FormatPath(path, MarmotFormatter::Options{ true, false });
	REQUIRE(written.m_files.size() == 1u);
	REQUIRE_FALSE(written.m_files.front().m_error.HasValue());
	CHECK(written.m_files.front().m_changed);

	const std::string formatted = ReadFile(path);
	CHECK(MarmotSource::StartsWithByteOrderMark(formatted));
	CHECK_THAT(formatted, ContainsSubstring("def main = fn() -> Int => 0;"));

	// Formatting it again changes nothing: the mark is not a difference.
	const MarmotFormatter::RunResult checked = MarmotFormatter::FormatPath(path, MarmotFormatter::Options{ false, true });
	REQUIRE(checked.m_files.size() == 1u);
	CHECK_FALSE(checked.m_files.front().m_changed);
}
