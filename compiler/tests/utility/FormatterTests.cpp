#include <catch2/catch_test_macros.hpp>

#include "Utility/Formatter/Formatter.h"

#include <expected>
#include <string>
#include <string_view>

namespace
{
	[[nodiscard]] std::string FormatOrFail(std::string_view source_code, std::string_view file_name)
	{
		const std::expected<std::string, CompilerError> format_result =
			MidoriFormatter::FormatSource(source_code, file_name);
		if (!format_result.has_value())
		{
			FAIL(std::string(format_result.error().Rendered()));
		}
		return format_result.value();
	}

	void RequireIdempotent(std::string_view source_code, std::string_view file_name)
	{
		const std::string first_pass = FormatOrFail(source_code, file_name);
		const std::string second_pass = FormatOrFail(first_pass, file_name);
		CHECK(first_pass == second_pass);
	}
}

TEST_CASE("Formatter preserves comments while normalizing spacing", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"// heading\n"
		"def main = fn()->Int=>{\n"
		"def value=/*inline*/1; // trailing\n"
		"value\n"
		"};\n";

	const std::string formatted = FormatOrFail(source_code, "CommentedFormat.mmt");

	const std::string expected =
		"module Main\n"
		"\n"
		"// heading\n"
		"def main = fn() -> Int => {\n"
		"    def value = /*inline*/ 1;  // trailing\n"
		"    value\n"
		"};\n";

	CHECK(formatted == expected);
}

TEST_CASE("Formatter is idempotent for comment-bearing files", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"/* banner */ def main = fn()->Int=>0;\n";

	RequireIdempotent(source_code, "FormatterIdempotent.mmt");
}

TEST_CASE("Formatter handles an empty input", "[formatter][edge]")
{
	const std::string formatted = FormatOrFail("", "Empty.mmt");
	CHECK(formatted.empty());
	RequireIdempotent("", "Empty.mmt");
}

TEST_CASE("Formatter handles a whitespace-only input", "[formatter][edge]")
{
	const std::string formatted = FormatOrFail("\n\n\n   \t\n", "Whitespace.mmt");
	CHECK(formatted.empty());
	RequireIdempotent("\n\n\n   \t\n", "Whitespace.mmt");
}

TEST_CASE("Formatter handles a comment-only file", "[formatter][edge][comments]")
{
	const std::string source_code =
		"// first\n"
		"// second\n"
		"/* block */\n";

	const std::string formatted = FormatOrFail(source_code, "CommentsOnly.mmt");

	CHECK(formatted ==
		"// first\n"
		"// second\n"
		"/* block */\n");
	RequireIdempotent(source_code, "CommentsOnly.mmt");
}

TEST_CASE("Formatter is idempotent for deeply nested expressions", "[formatter][edge]")
{
	const std::string source_code =
		"module DeepNest\n"
		"def deep = fn() -> Int => ((((((((((1 + 2))))))))));\n";

	const std::string formatted = FormatOrFail(source_code, "DeepNest.mmt");
	CHECK(formatted.find("(((((((((1 + 2))))))))") != std::string::npos);
	RequireIdempotent(source_code, "DeepNest.mmt");
}

TEST_CASE("Formatter preserves long expression lines without wrapping", "[formatter][edge]")
{
	const std::string long_addition =
		"def total = 1111111111 + 2222222222 + 3333333333 + 4444444444 + 5555555555 + 6666666666;\n";
	const std::string source_code = std::string("module Long\n") + long_addition;

	const std::string formatted = FormatOrFail(source_code, "Long.mmt");
	CHECK(formatted.find("1111111111 + 2222222222 + 3333333333") != std::string::npos);
	RequireIdempotent(source_code, "Long.mmt");
}

TEST_CASE("Formatter normalizes record field spacing and is idempotent", "[formatter][edge]")
{
	const std::string source_code =
		"module Shapes\n"
		"type Point={x:Int,y:Int};\n";

	const std::string formatted = FormatOrFail(source_code, "Shapes.mmt");
	CHECK(formatted.find("type Point = {") != std::string::npos);
	CHECK(formatted.find("x: Int,") != std::string::npos);
	RequireIdempotent(source_code, "Shapes.mmt");
}

TEST_CASE("Formatter is idempotent for match arms", "[formatter][edge]")
{
	const std::string source_code =
		"module MatchArms\n"
		"def value = match 0 with case 0 => 0 case 1 => 1 case _ => 2;\n";

	const std::string first_pass = FormatOrFail(source_code, "MatchArms.mmt");
	CHECK(first_pass.find("case 0 => 0") != std::string::npos);
	CHECK(first_pass.find("case _ => 2") != std::string::npos);
	RequireIdempotent(source_code, "MatchArms.mmt");
}

TEST_CASE("Formatter round-trips guarded match arms", "[formatter][edge]")
{
	const std::string source_code =
		"module GuardedArms\n"
		"def value = match 0 with case n if n>10 =>1 case n if n>0 =>2 case _ => 3;\n";

	const std::string first_pass = FormatOrFail(source_code, "GuardedArms.mmt");
	CHECK(first_pass.find("case n if n > 10 => 1") != std::string::npos);
	CHECK(first_pass.find("case n if n > 0 => 2") != std::string::npos);
	CHECK(first_pass.find("case _ => 3") != std::string::npos);
	RequireIdempotent(source_code, "GuardedArms.mmt");
}

TEST_CASE("Formatter keeps inline comments inline between tokens", "[formatter][comments]")
{
	const std::string source_code =
		"module Inline\n"
		"def x = 1 /* keep */ + 2;\n";

	const std::string formatted = FormatOrFail(source_code, "Inline.mmt");
	CHECK(formatted.find("1 /* keep */ + 2") != std::string::npos);
	RequireIdempotent(source_code, "Inline.mmt");
}

TEST_CASE("Formatter preserves trailing line comments after writing once", "[formatter][comments]")
{
	const std::string source_code =
		"module Trail\n"
		"def x = 1; // tail\n"
		"def y = 2;\n";

	const std::string first_pass = FormatOrFail(source_code, "Trail.mmt");
	CHECK(first_pass.find("// tail") != std::string::npos);
	RequireIdempotent(source_code, "Trail.mmt");
}

TEST_CASE("Formatter normalizes string escape sequences", "[formatter][edge]")
{
	const std::string source_code =
		"module Strings\n"
		"def s = \"a\nb\tc\";\n";

	const std::string formatted = FormatOrFail(source_code, "Strings.mmt");
	CHECK(formatted.find("\"a\\nb\\tc\"") != std::string::npos);
	RequireIdempotent(source_code, "Strings.mmt");
}

TEST_CASE("Formatter round-trips newtype declarations and leading-bar sums", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"type Meters=Int;\n"
		"type Solo=|Only(Int);\n";

	const std::string formatted = FormatOrFail(source_code, "NewtypeFormat.mmt");

	const std::string expected =
		"module Main\n"
		"\n"
		"type Meters = Int;\n"
		"\n"
		"type Solo = | Only(Int);\n";

	CHECK(formatted == expected);
}

TEST_CASE("Formatter is idempotent for newtype declarations", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"type Meters = Int;\n"
		"type Solo = | Only(Int);\n";

	const std::string once = FormatOrFail(source_code, "NewtypeIdempotent.mmt");
	const std::string twice = FormatOrFail(once, "NewtypeIdempotent.mmt");

	CHECK(once == twice);
}

TEST_CASE("Formatter spaces a parenthesis after an infix operator but not after a unary one", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"def a = 10 /(5 - 5);\n"
		"def b = \"x\" ++(\"y\" ++ \"z\");\n"
		"def c = -(1 - 2) + - 3;\n"
		"def (d, e) = (1, 2);\n"
		"def f = if(a == 0) && true then 1 else 2;\n";

	const std::string formatted = FormatOrFail(source_code, "OperandParens.mmt");
	CHECK(formatted.find("def a = 10 / (5 - 5);") != std::string::npos);
	CHECK(formatted.find("def b = \"x\" ++ (\"y\" ++ \"z\");") != std::string::npos);
	CHECK(formatted.find("def c = -(1 - 2) + -3;") != std::string::npos);
	CHECK(formatted.find("def (d, e) = (1, 2);") != std::string::npos);
	CHECK(formatted.find("def f = if (a == 0) && true then 1 else 2;") != std::string::npos);
	RequireIdempotent(source_code, "OperandParens.mmt");
}

TEST_CASE("Formatter writes type arguments without spaces and comparisons with them", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"import { <IO>, <Math.Vector> }\n"
		"type Holder = { entries: Array < Map::Map < Text, Array < Int > > >, count: Int };\n"
		"def Pick = fn < T >(values: Array < T >, limit: Int) -> Bool => (#values < limit) && (limit > 0);\n";

	const std::string formatted = FormatOrFail(source_code, "TypeArguments.mmt");
	CHECK(formatted.find("import { <IO>, <Math.Vector> }\n") != std::string::npos);
	CHECK(formatted.find("    entries: Array<Map::Map<Text, Array<Int>>>,\n") != std::string::npos);
	CHECK(formatted.find("def Pick = fn<T>(values: Array<T>, limit: Int) -> Bool => (#values < limit) && (limit > 0);") != std::string::npos);
	RequireIdempotent(source_code, "TypeArguments.mmt");
}

TEST_CASE("Formatter separates declarations by one blank line and keeps the author's", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"import { \"./A.mmt\" }\n"
		"def a = 1;\n"
		"// about b\n"
		"def b = 2;\n"
		"IO::PrintLine(\"one\");\n"
		"\n"
		"\n"
		"\n"
		"IO::PrintLine(\"two\");\n";

	const std::string formatted = FormatOrFail(source_code, "BlankLines.mmt");

	const std::string expected =
		"module Main\n"
		"import { \"./A.mmt\" }\n"
		"\n"
		"def a = 1;\n"
		"\n"
		"// about b\n"
		"def b = 2;\n"
		"IO::PrintLine(\"one\");\n"
		"\n"
		"IO::PrintLine(\"two\");\n";

	CHECK(formatted == expected);
	RequireIdempotent(source_code, "BlankLines.mmt");
}

TEST_CASE("Formatter indents a match arm's block from its case", "[formatter]")
{
	const std::string source_code =
		"module Main\n"
		"def f = fn(x: Int) -> Int => match x with\n"
		"case 0 => {\n"
		"def y = 1;\n"
		"y\n"
		"}\n"
		"case _ => 2;\n"
		"def g = 3;\n";

	const std::string formatted = FormatOrFail(source_code, "ArmBlock.mmt");

	const std::string expected =
		"module Main\n"
		"\n"
		"def f = fn(x: Int) -> Int => match x with\n"
		"    case 0 => {\n"
		"        def y = 1;\n"
		"        y\n"
		"    }\n"
		"    case _ => 2;\n"
		"\n"
		"def g = 3;\n";

	CHECK(formatted == expected);
	RequireIdempotent(source_code, "ArmBlock.mmt");
}
