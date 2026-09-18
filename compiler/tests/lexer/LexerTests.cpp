#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Token/Token.h"
#include "support/CompileHelpers.h"

#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::ContainsSubstring;

namespace
{
	const Token* FindTokenByLexeme(const TokenStream& tokens, std::string_view lexeme)
	{
		for (int index = 0; index < tokens.Size(); index += 1)
		{
			const Token& token = tokens[index];
			if (token.m_lexeme == lexeme)
			{
				return &token;
			}
		}

		return nullptr;
	}
}

TEST_CASE("Lexer tokenizes literal variants and decodes string escapes", "[lexer]")
{
	const std::string source_code =
		R"(def values = [42, 0x2A, 0b1010, 3.5, "line\n\t\"\\q"];
)";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "LiteralForms.mmt");
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const TokenStream& tokens = lex_result->m_tokens;
	const std::vector<Token::Name> expected_names
	{
		Token::Name::DEF,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::LEFT_BRACKET,
		Token::Name::INTEGER_LITERAL,
		Token::Name::COMMA,
		Token::Name::INTEGER_LITERAL,
		Token::Name::COMMA,
		Token::Name::INTEGER_LITERAL,
		Token::Name::COMMA,
		Token::Name::FLOAT_LITERAL,
		Token::Name::COMMA,
		Token::Name::TEXT_LITERAL,
		Token::Name::RIGHT_BRACKET,
		Token::Name::SINGLE_SEMICOLON
	};

	REQUIRE(MidoriTest::CollectTokenNames(tokens) == expected_names);
	CHECK(tokens[4].m_lexeme == "42");
	CHECK(tokens[6].m_lexeme == "0x2A");
	CHECK(tokens[8].m_lexeme == "0b1010");
	CHECK(tokens[10].m_lexeme == "3.5");
	CHECK(tokens[12].m_lexeme == "line\n\t\"\\q");
}

TEST_CASE("Lexer skips comments and preserves token line numbers", "[lexer]")
{
	const std::string source_code =
		R"(module Commented
// this line comment should disappear
def value = 1;
/* block comment
still in the comment */
def other = 2;
)";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "Comments.mmt");
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const TokenStream& tokens = lex_result->m_tokens;
	const std::vector<Token::Name> expected_names
	{
		Token::Name::MODULE,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::DEF,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::DEF,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON
	};

	REQUIRE(MidoriTest::CollectTokenNames(tokens) == expected_names);

	const Token* other_token = FindTokenByLexeme(tokens, "other");
	REQUIRE(other_token != nullptr);
	CHECK(other_token->m_line == 6);
}

TEST_CASE("Lexer preserves comment tokens when requested", "[lexer]")
{
	const std::string source_code =
		R"(module Commented
// keep me
def value = /* inline */ 1;
)";

	MidoriResult::LexerResult lex_result = Lexer(
		std::string(source_code),
		"PreserveComments.mmt",
		Lexer::Options{ .m_preserve_comments = true }).Lex();
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const TokenStream& tokens = lex_result.value();
	REQUIRE(MidoriTest::CollectTokenNames(tokens, false, true) == std::vector<Token::Name>
	{
		Token::Name::MODULE,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::LINE_COMMENT,
		Token::Name::DEF,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::BLOCK_COMMENT,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::END_OF_FILE
	});

	const Token* line_comment = FindTokenByLexeme(tokens, "// keep me");
	const Token* block_comment = FindTokenByLexeme(tokens, "/* inline */");
	REQUIRE(line_comment != nullptr);
	REQUIRE(block_comment != nullptr);
	CHECK(line_comment->m_line == 2);
	CHECK(line_comment->m_column == 0);
	CHECK(block_comment->m_line == 3);
	CHECK(block_comment->m_column == 12);
}

TEST_CASE("Lexer records exact token columns and source spans", "[lexer]")
{
	const std::string source_code =
		R"(def alpha = alpha + alpha;
)";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "TokenSpans.mmt");
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const TokenStream& tokens = lex_result->m_tokens;
	REQUIRE(tokens.Size() == 8);

	CHECK(tokens[1].m_column == 4);
	CHECK(tokens[1].m_source_length == 5u);
	CHECK(tokens[3].m_column == 12);
	CHECK(tokens[3].m_source_length == 5u);
	CHECK(tokens[5].m_column == 20);
	CHECK(tokens[5].m_source_length == 5u);
	CHECK(tokens[7].m_token_name == Token::Name::END_OF_FILE);
	CHECK(tokens[7].m_column == 26);
	CHECK(tokens[7].m_source_length == 0u);
}

TEST_CASE("Lexer recognizes compound operator tokens used by later stages", "[lexer]")
{
	const std::string source_code =
		R"(value =++ next;
total += 1;
bits << 2;
bits >> 1;
mask <<= 2;
flags >>= 3;
inverted = ~mask;
stream |> sink;
)";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "Operators.mmt");
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const std::vector<Token::Name> expected_names
	{
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::EQUAL_PLUS_PLUS,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::PLUS_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::LEFT_SHIFT,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::RIGHT_SHIFT,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::LEFT_SHIFT_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::RIGHT_SHIFT_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::TILDE,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::BAR_BRACKET,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_SEMICOLON
	};

	const TokenStream& tokens = lex_result->m_tokens;
	const Token* left_shift_token = FindTokenByLexeme(tokens, "<<");
	const Token* right_shift_token = FindTokenByLexeme(tokens, ">>");
	const Token* left_shift_equal_token = FindTokenByLexeme(tokens, "<<=");
	const Token* right_shift_equal_token = FindTokenByLexeme(tokens, ">>=");
	const Token* tilde_token = FindTokenByLexeme(tokens, "~");

	REQUIRE(MidoriTest::CollectTokenNames(tokens) == expected_names);
	REQUIRE(left_shift_token != nullptr);
	REQUIRE(right_shift_token != nullptr);
	REQUIRE(left_shift_equal_token != nullptr);
	REQUIRE(right_shift_equal_token != nullptr);
	REQUIRE(tilde_token != nullptr);
	CHECK(left_shift_token->m_token_name == Token::Name::LEFT_SHIFT);
	CHECK(right_shift_token->m_token_name == Token::Name::RIGHT_SHIFT);
	CHECK(left_shift_equal_token->m_token_name == Token::Name::LEFT_SHIFT_EQUAL);
	CHECK(right_shift_equal_token->m_token_name == Token::Name::RIGHT_SHIFT_EQUAL);
	CHECK(tilde_token->m_token_name == Token::Name::TILDE);
}

TEST_CASE("Lexer reports dedicated migration diagnostics for legacy shift operators", "[lexer]")
{
	struct LegacyShiftCase
	{
		std::string_view m_legacy_operator;
		std::string_view m_replacement_operator;
	};

	const std::vector<LegacyShiftCase> cases
	{
		{"<~", "<<"},
		{"<~=", "<<="},
		{"~>", ">>"},
		{"~>=", ">>="}
	};

	for (const LegacyShiftCase& test_case : cases)
	{
		CAPTURE(test_case.m_legacy_operator);

		const std::string source_code = "def value = bits " + std::string(test_case.m_legacy_operator) + " 1;\n";
		std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "LegacyShiftSyntax.mmt");

		REQUIRE_FALSE(lex_result.has_value());

		const CompilerError& error = lex_result.error();
		const std::string expected_message = "Legacy shift operator '" + std::string(test_case.m_legacy_operator) + "' is no longer supported.";
		const std::string expected_suggestion = "Use '" + std::string(test_case.m_replacement_operator) + "' instead.";

		REQUIRE(error.m_stage == CompilerStage::Lexer);
		REQUIRE(error.m_location.has_value());
		REQUIRE(error.m_suggestion.has_value());
		CHECK(error.m_location->m_file_name == "LegacyShiftSyntax.mmt");
		CHECK(error.m_location->m_line == 1);
		CHECK(error.m_message == expected_message);
		CHECK(*error.m_suggestion == expected_suggestion);

		const std::string rendered_error = std::string(error.Rendered());
		CHECK_THAT(rendered_error, ContainsSubstring("Lexer Error"));
		CHECK_THAT(rendered_error, ContainsSubstring(expected_message));
		CHECK_THAT(rendered_error, ContainsSubstring(expected_suggestion));
		CHECK_THAT(rendered_error, ContainsSubstring("def value = bits " + std::string(test_case.m_legacy_operator) + " 1;"));
	}
}

TEST_CASE("Lexer reports the dedicated =+ typo diagnostic", "[lexer]")
{
	const std::string source_code =
		R"(def value =+ 1;
)";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "EqualPlusTypo.mmt");

	REQUIRE_FALSE(lex_result.has_value());

	const CompilerError& error = lex_result.error();
	REQUIRE(error.m_stage == CompilerStage::Lexer);
	REQUIRE(error.m_location.has_value());
	CHECK(error.m_location->m_file_name == "EqualPlusTypo.mmt");
	CHECK(error.m_location->m_line == 1);

	const std::string rendered_error = std::string(error.Rendered());
	CHECK_THAT(rendered_error, ContainsSubstring("Lexer Error"));
	CHECK_THAT(rendered_error, ContainsSubstring("Unexpected character '=+' (did you mean '=++'?)"));
	CHECK_THAT(rendered_error, ContainsSubstring("EqualPlusTypo.mmt:1"));
	CHECK_THAT(rendered_error, ContainsSubstring("def value =+ 1;"));
}
