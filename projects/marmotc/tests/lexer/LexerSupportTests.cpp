#include <catch2/catch_test_macros.hpp>

#include "Compiler/Token/Token.h"
#include "support/CompileHelpers.h"
#include "Support/Expected.h"

TEST_CASE("LexSnippet returns concise token sequences for expression tests", "[lexer][support]")
{
	const std::string source_code =
		"def value = 1 + 2 * 3;\n";

	MarmotCompiler::Expected<MarmotTest::LexedSnippet, CompilerError> lex_result = MarmotTest::LexSnippet(source_code, "LexerSupport.mmt");
	if (!lex_result.HasValue())
	{
		FAIL(std::string(lex_result.Error().Rendered()));
	}

	const std::vector<Token::Name> token_names = MarmotTest::CollectTokenNames(lex_result.Value().m_tokens);
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
