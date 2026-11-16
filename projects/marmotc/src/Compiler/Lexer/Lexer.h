#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Result/Result.h"

#include <string>
#include <string_view>
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
	};

	Source m_source;
	Cursor m_cursor;
	Options m_options;

public:

	Lexer(std::string&& source_code, std::string_view file_name) noexcept;
	Lexer(std::string&& source_code, std::string_view file_name, Options options) noexcept;

	MarmotResult::LexerResult Lex() &;

	MarmotResult::LexerResult Lex() &&;

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

	MarmotResult::TokenResult MakeTokenResult(Token::Name type) const;

	MarmotResult::TokenResult MakeTokenResult(Token::Name type, std::string&& lexeme) const;

	MarmotResult::TokenResult MakeEndOfFileTokenResult() const;

	MarmotResult::Result<LexState> RecordTokenOrError(LexState state);

	MarmotResult::TokenResult LexOneToken();

	MarmotResult::TokenResult LexTokenAfterWhitespace();

	MarmotResult::TokenResult LexTokenFrom(char next_char);

	MarmotResult::TokenResult MatchDot();

	MarmotResult::TokenResult MatchPlus();

	MarmotResult::TokenResult MatchMinus();

	MarmotResult::TokenResult MatchColon();

	MarmotResult::TokenResult MatchPercent();

	MarmotResult::TokenResult MatchStar();

	MarmotResult::TokenResult MatchSlash();

	MarmotResult::TokenResult MatchLineComment();

	MarmotResult::TokenResult MatchBlockComment();

	MarmotResult::TokenResult MatchPipe();

	MarmotResult::TokenResult MatchCaret();

	MarmotResult::TokenResult MatchAmpersand();

	MarmotResult::TokenResult MatchBang();

	MarmotResult::TokenResult MatchEqual();


	MarmotResult::TokenResult MatchGreater();

	MarmotResult::TokenResult MatchLess();

	MarmotResult::TokenResult MatchLeftShift();

	MarmotResult::TokenResult MatchTilde();

	MarmotResult::TokenResult MatchRightShift();


	MarmotResult::TokenResult MatchLiteralOrIdentifier(char next_char);

	MarmotResult::TokenResult MakeInvalidCharacterError(char next_char) const;

	MarmotResult::VoidResult SkipWhitespaceAndComments();

	MarmotResult::TokenResult MatchString();

	MarmotResult::TokenResult MatchNumber();

	MarmotResult::TokenResult MatchDecimalNumber();

	MarmotResult::TokenResult MatchPrefixedInteger(bool (*predicate)(char));

	bool IsHexPrefix() const;

	bool IsBinaryPrefix() const;

	static bool IsHexDigit(char c);

	static bool IsBinaryDigit(char c);

	MarmotResult::TokenResult MatchIdentifierOrReserved();

	template<typename Predicate>
	int ConsumeWhile(Predicate&& pred);

	MarmotResult::TokenResult MatchStringRecursive(std::string&& acc);

	MarmotResult::VoidResult SkipLineComment();

	MarmotResult::VoidResult SkipBlockComment();

	int ConsumeDigits();

	int ConsumeAlphaNumeric();

	MarmotResult::LexerResult LexRecursive(LexState state);

	static Source BuildSource(std::string&& source_code, std::string_view file_name);

	static std::vector<std::string> SplitIntoLines(const std::string& source);
};
