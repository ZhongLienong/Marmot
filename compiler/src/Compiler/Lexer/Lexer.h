#pragma once

#include "Common/Error/Error.h"
#include "Compiler/Result/Result.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class Lexer
{
public:
	struct Options
	{
		bool m_preserve_comments = false;
	};

private:
	struct Source
	{
		const std::string m_code;
		const std::string m_file_name;
		const std::vector<std::string> m_lines;
	};

	struct Cursor
	{
		int m_current = 0;
		int m_begin = 0;
		int m_begin_line_start = 0;
		int m_line_start = 0;
		int m_line = 1;
		int m_begin_line = 1;
		int m_last_token_end_line = 1;
		int m_last_token_end_column = 0;
		bool m_has_last_token_end = false;
	};

	struct LexState
	{
		TokenStream m_tokens;
		std::vector<CompilerError> m_errors;
	};

	Source m_source;
	Cursor m_cursor;
	Options m_options;
	static const std::unordered_map<std::string, Token::Name> s_keywords;

public:

	Lexer(std::string&& source_code, std::string_view file_name) noexcept;
	Lexer(std::string&& source_code, std::string_view file_name, Options options) noexcept;

	MidoriResult::LexerResult Lex() &;

	MidoriResult::LexerResult Lex() &&;

private:

	bool IsAtEnd(int offset) const;

	static bool IsDigit(char c);

	static bool IsAlpha(char c);

	static bool IsAlphaNumeric(char c);

	char Advance();

	char LookAhead(int offset) const;

	bool MatchNext(char expected);

	Lexer& BeginToken();

	Lexer& AdvanceLine();

	int CurrentColumn() const;

	int BeginColumn() const;

	Token MakeToken(Token::Name type) const;

	Token MakeToken(Token::Name type, std::string&& lexeme) const;

	Token MakeEndOfFileToken() const;

	MidoriResult::TokenResult MakeTokenResult(Token::Name type) const;

	MidoriResult::TokenResult MakeTokenResult(Token::Name type, std::string&& lexeme) const;

	MidoriResult::TokenResult MakeEndOfFileTokenResult() const;

	MidoriResult::Result<LexState> RecordTokenOrError(LexState state);

	MidoriResult::TokenResult LexOneToken();

	MidoriResult::TokenResult LexTokenAfterWhitespace();

	MidoriResult::TokenResult LexTokenFrom(char next_char);

	MidoriResult::TokenResult MatchDot();

	MidoriResult::TokenResult MatchPlus();

	MidoriResult::TokenResult MatchMinus();

	MidoriResult::TokenResult MatchColon();

	MidoriResult::TokenResult MatchPercent();

	MidoriResult::TokenResult MatchStar();

	MidoriResult::TokenResult MatchSlash();

	MidoriResult::TokenResult MatchLineComment();

	MidoriResult::TokenResult MatchBlockComment();

	MidoriResult::TokenResult MatchPipe();

	MidoriResult::TokenResult MatchCaret();

	MidoriResult::TokenResult MatchAmpersand();

	MidoriResult::TokenResult MatchBang();

	MidoriResult::TokenResult MatchEqual();

	MidoriResult::TokenResult MatchEqualPlusPlus();

	MidoriResult::TokenResult MatchGreater();

	MidoriResult::TokenResult MatchLess();

	MidoriResult::TokenResult MatchLeftShift();

	MidoriResult::TokenResult MatchTilde();

	MidoriResult::TokenResult MatchRightShift();

	MidoriResult::TokenResult MakeLegacyShiftSyntaxError(std::string_view legacy_operator, std::string_view replacement_operator) const;

	MidoriResult::TokenResult MatchLiteralOrIdentifier(char next_char);

	MidoriResult::TokenResult MakeInvalidCharacterError(char next_char) const;

	MidoriResult::VoidResult SkipWhitespaceAndComments();

	MidoriResult::TokenResult MatchString();

	MidoriResult::TokenResult MatchNumber();

	MidoriResult::TokenResult MatchDecimalNumber();

	MidoriResult::TokenResult MatchPrefixedInteger(bool (*predicate)(char));

	bool IsHexPrefix() const;

	bool IsBinaryPrefix() const;

	static bool IsHexDigit(char c);

	static bool IsBinaryDigit(char c);

	MidoriResult::TokenResult MatchIdentifierOrReserved();

	template<typename Predicate>
	int ConsumeWhile(Predicate&& pred);

	MidoriResult::TokenResult MatchStringRecursive(std::string&& acc);

	MidoriResult::VoidResult SkipLineComment();

	MidoriResult::VoidResult SkipBlockComment();

	int ConsumeDigits();

	int ConsumeAlphaNumeric();

	MidoriResult::LexerResult LexRecursive(LexState state);

	static Source BuildSource(std::string&& source_code, std::string_view file_name);

	static std::vector<std::string> SplitIntoLines(const std::string& source);
};
