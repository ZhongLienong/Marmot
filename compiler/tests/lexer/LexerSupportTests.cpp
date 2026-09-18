#include <catch2/catch_test_macros.hpp>

#include "Compiler/Token/Token.h"
#include "support/CompileHelpers.h"

TEST_CASE("LexSnippet returns concise token sequences for expression tests", "[lexer][support]")
{
	const std::string source_code =
		"def value = 1 + 2 * 3;\n";

	std::expected<MidoriTest::LexedSnippet, CompilerError> lex_result = MidoriTest::LexSnippet(source_code, "LexerSupport.mmt");
	if (!lex_result.has_value())
	{
		FAIL(std::string(lex_result.error().Rendered()));
	}

	const std::vector<Token::Name> token_names = MidoriTest::CollectTokenNames(lex_result.value().m_tokens);
	const std::vector<Token::Name> expected_names
	{
		Token::Name::DEF,
		Token::Name::IDENTIFIER_LITERAL,
		Token::Name::SINGLE_EQUAL,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_PLUS,
		Token::Name::INTEGER_LITERAL,
		Token::Name::STAR,
		Token::Name::INTEGER_LITERAL,
		Token::Name::SINGLE_SEMICOLON
	};

	REQUIRE(token_names == expected_names);
}
