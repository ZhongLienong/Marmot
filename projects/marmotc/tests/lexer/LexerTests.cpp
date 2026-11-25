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

TEST_CASE("Lexer recognizes the reserved vocabulary without reserving contextual names", "[lexer]")
{
	const std::string source_code =
		"Float Int Byte Word Text Bool Unit Array Never Range Worker Channel "
		"else false for in if true def fn as import foreign case then with match "
		"module export public private use class instance where type alias deriving "
		"ref Ref from moduleName ArrayUtil";
	const std::vector<Token::Name> expected_names
	{
		Token::Name::FLOAT, Token::Name::INTEGER, Token::Name::BYTE, Token::Name::WORD,
		Token::Name::TEXT, Token::Name::BOOL, Token::Name::UNIT, Token::Name::ARRAY,
		Token::Name::NEVER, Token::Name::RANGE, Token::Name::WORKER, Token::Name::CHANNEL_TYPE,
		Token::Name::ELSE, Token::Name::FALSE, Token::Name::FOR, Token::Name::IN,
		Token::Name::IF, Token::Name::TRUE, Token::Name::DEF, Token::Name::FUNCTION,
		Token::Name::AS, Token::Name::IMPORT, Token::Name::FOREIGN, Token::Name::CASE,
		Token::Name::THEN, Token::Name::WITH, Token::Name::MATCH, Token::Name::MODULE,
		Token::Name::EXPORT, Token::Name::PUBLIC, Token::Name::PRIVATE, Token::Name::USE,
		Token::Name::CLASS, Token::Name::INSTANCE, Token::Name::WHERE, Token::Name::TYPE,
		Token::Name::ALIAS, Token::Name::DERIVING,
		Token::Name::IDENTIFIER_LITERAL, Token::Name::IDENTIFIER_LITERAL,
		Token::Name::IDENTIFIER_LITERAL, Token::Name::IDENTIFIER_LITERAL, Token::Name::IDENTIFIER_LITERAL
	};
	const MarmotResult::Result<MarmotTest::LexedSnippet> lexed = MarmotTest::LexSnippet(source_code);
	REQUIRE(lexed.HasValue());
	CHECK(MarmotTest::CollectTokenNames(lexed->m_tokens) == expected_names);
}

TEST_CASE("Synthetic fixed tokens agree with source spellings and preserve their anchor", "[lexer][token]")
{
	const std::string source_code =
		"-> => ( ) { } [ ] , . .. ; + ++ - << >> % * / | || |> ^ & && ! != = == > >= < <= : :: := ~ # "
		"Float Int Byte Word Text Bool Unit Array Never Range Worker Channel "
		"else false for in if true def fn as import foreign case then with match "
		"module export public private use class instance where type alias deriving";
	const MarmotResult::Result<MarmotTest::LexedSnippet> lexed = MarmotTest::LexSnippet(source_code);
	REQUIRE(lexed.HasValue());
	const Token anchor("Example", Token::Name::IDENTIFIER_LITERAL, 17, "Generated.mmt", 8, 7u);
	for (int index = 0; index < lexed->m_tokens.Size(); index += 1)
	{
		const Token& source_token = lexed->m_tokens[index];
		if (source_token.m_token_name == Token::Name::WHITESPACE || source_token.m_token_name == Token::Name::END_OF_FILE)
		{
			continue;
		}
		INFO(source_token.m_lexeme);
		const Token generated(source_token.m_token_name, anchor);
		CHECK(generated.m_lexeme == source_token.m_lexeme);
		CHECK(generated.m_token_name == source_token.m_token_name);
		CHECK(generated.m_file_name == anchor.m_file_name);
		CHECK(generated.m_line == anchor.m_line);
		CHECK(generated.m_column == anchor.m_column);
		CHECK(generated.m_source_length == anchor.m_source_length);
	}
	const Token contextual_ref(Token::Name::REF, anchor);
	CHECK(contextual_ref.m_lexeme == "ref");
	CHECK_FALSE(Token::IsKeyword(contextual_ref.m_token_name));
}

TEST_CASE("Lexer tokenizes literal variants and decodes string escapes", "[lexer]")
{
	const std::string source_code =
		R"(def values = [42, 0x2A, 0b1010, 3.5, "line\n\t\"\\q"];
)";

	MarmotResult::Result<MarmotTest::LexedSnippet> lex_result = MarmotTest::LexSnippet(source_code, "LiteralForms.mmt");
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
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

	REQUIRE(MarmotTest::CollectTokenNames(tokens) == expected_names);
	CHECK(tokens[4].m_lexeme == "42");
	CHECK(tokens[6].m_lexeme == "0x2A");
	CHECK(tokens[8].m_lexeme == "0b1010");
	CHECK(tokens[10].m_lexeme == "3.5");
	CHECK(tokens[12].m_lexeme == "line\n\t\"\\q");
}

TEST_CASE("Text escapes preserve bytes and reject malformed sequences", "[lexer]")
{
	const MarmotResult::Result<MarmotTest::LexedSnippet> escaped =
		MarmotTest::LexSnippet(R"("\r\0\x41\u{A2}\u{20AC}\u{1F600}\u{10FFFF}")");
	REQUIRE(escaped.HasValue());
	CHECK(escaped->m_tokens[0].m_lexeme == std::string("\r\0" "A\xC2\xA2\xE2\x82\xAC\xF0\x9F\x98\x80\xF4\x8F\xBF\xBF", 16));

	for (const std::string_view source : { R"("\q")", R"("\x0")", R"("\u{}")", R"("\u{123")", R"("\u{D800}")", R"("\u{110000}")" })
	{
		const MarmotResult::Result<MarmotTest::LexedSnippet> invalid = MarmotTest::LexSnippet(std::string(source));
		REQUIRE_FALSE(invalid.HasValue());
		REQUIRE(invalid.Error().m_location.HasValue());
		CHECK(invalid.Error().m_location->m_column == 1);
	}
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

	MarmotResult::Result<MarmotTest::LexedSnippet> lex_result = MarmotTest::LexSnippet(source_code, "Comments.mmt");
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
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

	REQUIRE(MarmotTest::CollectTokenNames(tokens) == expected_names);

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

	MarmotResult::LexerResult lex_result = Lexer(
		std::string(source_code),
		"PreserveComments.mmt",
		Lexer::Options{ .m_preserve_comments = true }).Lex();
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
	}

	const TokenStream& tokens = lex_result.Value();
	REQUIRE(MarmotTest::CollectTokenNames(tokens, false, true) == std::vector<Token::Name>
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

	MarmotResult::Result<MarmotTest::LexedSnippet> lex_result = MarmotTest::LexSnippet(source_code, "TokenSpans.mmt");
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
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
		R"(bits << 2;
bits >> 1;
inverted = ~mask;
stream |> sink;
joined = left ++ right;
)";

	MarmotResult::Result<MarmotTest::LexedSnippet> lex_result = MarmotTest::LexSnippet(source_code, "Operators.mmt");
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
	}

	const std::vector<Token::Name> expected_names
	{
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::LEFT_SHIFT,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::RIGHT_SHIFT,
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
		Token::Name::SINGLE_SEMICOLON,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::DOUBLE_PLUS,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_SEMICOLON
	};

	const TokenStream& tokens = lex_result->m_tokens;
	const Token* left_shift_token = FindTokenByLexeme(tokens, "<<");
	const Token* right_shift_token = FindTokenByLexeme(tokens, ">>");
	const Token* concat_token = FindTokenByLexeme(tokens, "++");
	const Token* tilde_token = FindTokenByLexeme(tokens, "~");

	REQUIRE(MarmotTest::CollectTokenNames(tokens) == expected_names);
	REQUIRE(left_shift_token != nullptr);
	REQUIRE(right_shift_token != nullptr);
	REQUIRE(concat_token != nullptr);
	REQUIRE(tilde_token != nullptr);
	CHECK(left_shift_token->m_token_name == Token::Name::LEFT_SHIFT);
	CHECK(right_shift_token->m_token_name == Token::Name::RIGHT_SHIFT);
	CHECK(concat_token->m_token_name == Token::Name::DOUBLE_PLUS);
	CHECK(tilde_token->m_token_name == Token::Name::TILDE);
}


TEST_CASE("Lexer errors and tokens after a multi-line string point at their own columns", "[lexer]")
{
	const MarmotResult::Result<MarmotTest::LexedSnippet> invalid = MarmotTest::LexSnippet("def s = 1 $ 2;\ndef t = 1 ` 2;\n", "InvalidCharacter.mmt");
	REQUIRE_FALSE(invalid.HasValue());
	REQUIRE(invalid.Error().m_location.HasValue());
	CHECK(invalid.Error().m_location->m_line == 1);
	CHECK(invalid.Error().m_location->m_column == 10);

	const MarmotResult::Result<MarmotTest::LexedSnippet> unterminated = MarmotTest::LexSnippet("def a = 1;\ndef s = \"abc;\n", "UnterminatedString.mmt");
	REQUIRE_FALSE(unterminated.HasValue());
	REQUIRE(unterminated.Error().m_location.HasValue());
	CHECK(unterminated.Error().m_location->m_line == 2);
	CHECK(unterminated.Error().m_location->m_column == 8);

	const MarmotResult::Result<MarmotTest::LexedSnippet> unterminated_comment = MarmotTest::LexSnippet("def a = 1;\n  /* never closed\ndef b = 2;\n", "UnterminatedComment.mmt");
	REQUIRE_FALSE(unterminated_comment.HasValue());
	REQUIRE(unterminated_comment.Error().m_location.HasValue());
	CHECK(unterminated_comment.Error().m_location->m_line == 2);
	CHECK(unterminated_comment.Error().m_location->m_column == 2);

	const MarmotResult::Result<MarmotTest::LexedSnippet> multi_line = MarmotTest::LexSnippet("def s = \"abcdef\nb\"; def t = 1;\n", "MultiLineString.mmt");
	if (!multi_line.HasValue())
	{
		FAIL(std::string(multi_line.Error().Rendered()));
	}
	const Token* after_string = FindTokenByLexeme(multi_line->m_tokens, "t");
	REQUIRE(after_string != nullptr);
	CHECK(after_string->m_line == 2);
	CHECK(after_string->m_column == 8);
}
