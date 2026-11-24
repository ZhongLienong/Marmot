#include "Lexer.h"

#include "Compiler/Source/Source.h"

#include <sstream>

using namespace std::string_literals;

namespace
{
	unsigned HexDigitValue(char digit)
	{
		if (digit >= '0' && digit <= '9')
		{
			return static_cast<unsigned>(digit - '0');
		}
		return static_cast<unsigned>((digit | 0x20) - 'a' + 10);
	}

	void AppendUtf8(std::string& text, unsigned scalar)
	{
		const auto append = [&text](unsigned byte) { text += static_cast<char>(byte); };
		if (scalar <= 0x7F)
		{
			append(scalar);
			return;
		}
		if (scalar <= 0x7FF)
		{
			append(0xC0 | (scalar >> 6));
		}
		else
		{
			if (scalar <= 0xFFFF)
			{
				append(0xE0 | (scalar >> 12));
			}
			else
			{
				append(0xF0 | (scalar >> 18));
				append(0x80 | ((scalar >> 12) & 0x3F));
			}
			append(0x80 | ((scalar >> 6) & 0x3F));
		}
		append(0x80 | (scalar & 0x3F));
	}
}

template<typename Predicate>
int Lexer::ConsumeWhile(Predicate&& pred)
{
	int count = 0;
	while (!IsAtEnd(0) && pred(LookAhead(0)))
	{
		Advance();
		count += 1;
	}
	return count;
}

bool Lexer::IsAtEnd(int offset) const
{
	return static_cast<size_t>(m_cursor.m_current + offset) >= m_source.m_code.size();
}

bool Lexer::IsDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool Lexer::IsAlpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c == '_');
}

bool Lexer::IsAlphaNumeric(char c)
{
	return IsDigit(c) || IsAlpha(c);
}

char Lexer::Advance()
{
	return m_source.m_code[m_cursor.m_current++];
}

char Lexer::LookAhead(int offset) const
{
	return IsAtEnd(offset) ? '\0' : m_source.m_code[static_cast<size_t>(m_cursor.m_current + offset)];
}

bool Lexer::MatchNext(char expected)
{
	if (!IsAtEnd(0) && m_source.m_code[m_cursor.m_current] == expected)
	{
		m_cursor.m_current += 1;
		return true;
	}
	return false;
}

Lexer& Lexer::BeginToken()
{
	m_cursor.m_begin = m_cursor.m_current;
	m_cursor.m_begin_line = m_cursor.m_line;
	m_cursor.m_begin_line_start = m_cursor.m_line_start;
	return *this;
}

Lexer& Lexer::AdvanceLine()
{
	m_cursor.m_line += 1;
	Advance();
	m_cursor.m_line_start = m_cursor.m_current;
	return *this;
}

int Lexer::CurrentColumn() const
{
	return m_cursor.m_current - m_cursor.m_line_start;
}

int Lexer::BeginColumn() const
{
	return m_cursor.m_begin - m_cursor.m_begin_line_start;
}

Token Lexer::MakeToken(Token::Name type) const
{
	return Token
	(
		m_source.m_code.substr(m_cursor.m_begin, m_cursor.m_current - m_cursor.m_begin),
		type,
		m_cursor.m_begin_line,
		m_source.m_file_name,
		BeginColumn(),
		static_cast<size_t>(m_cursor.m_current - m_cursor.m_begin)
	);
}

Token Lexer::MakeToken(Token::Name type, std::string&& lexeme) const
{
	return Token
	(
		std::move(lexeme),
		type,
		m_cursor.m_begin_line,
		m_source.m_file_name,
		BeginColumn(),
		static_cast<size_t>(m_cursor.m_current - m_cursor.m_begin)
	);
}

Token Lexer::MakeEndOfFileToken() const
{
	const int line = m_cursor.m_has_last_token_end ? m_cursor.m_last_token_end_line : m_cursor.m_line;
	const int column = m_cursor.m_has_last_token_end ? m_cursor.m_last_token_end_column : CurrentColumn();
	return Token(std::string{}, Token::Name::END_OF_FILE, line, m_source.m_file_name, column, 0u);
}

MarmotResult::TokenResult Lexer::MakeTokenResult(Token::Name type) const
{
	return MakeToken(type);
}

MarmotResult::TokenResult Lexer::MakeTokenResult(Token::Name type, std::string&& lexeme) const
{
	return MakeToken(type, std::move(lexeme));
}

MarmotResult::TokenResult Lexer::MakeEndOfFileTokenResult() const
{
	return MakeEndOfFileToken();
}

int Lexer::ConsumeDigits()
{
	return ConsumeWhile(IsDigit);
}

int Lexer::ConsumeAlphaNumeric()
{
	return ConsumeWhile(IsAlphaNumeric);
}

MarmotResult::VoidResult Lexer::SkipLineComment()
{
	ConsumeWhile([](char c) { return c != '\n'; });
	return {};
}

MarmotResult::VoidResult Lexer::SkipBlockComment()
{
	while (true)
	{
		if (LookAhead(0) == '*' && LookAhead(1) == '/')
		{
			Advance(); // '*'
			Advance(); // '/'
			return {};
		}

		if (IsAtEnd(0))
		{
			return std::unexpected(MarmotError::GenerateLexerErrorWithContext("Unterminated block comment", m_cursor.m_begin_line, BeginColumn(), m_source.m_file_name, m_source.m_lines));
		}

		if (LookAhead(0) == '\n')
		{
			AdvanceLine();
			continue;
		}

		Advance();
	}
}

MarmotResult::VoidResult Lexer::SkipWhitespaceAndComments()
{
	while (true)
	{
		if (IsAtEnd(0))
		{
			return {};
		}

		const char c = LookAhead(0);

		switch (c)
		{
		case ' ':
		case '\r':
		case '\t':
			Advance();
			continue;
		case '\n':
			AdvanceLine();
			continue;
		case '/':
		{
			if (LookAhead(1) == '/')
			{
				if (m_options.m_preserve_comments)
				{
					return {};
				}

				Advance(); // '/'
				Advance(); // '/'
				return SkipLineComment()
					.and_then([this]() -> MarmotResult::VoidResult { return SkipWhitespaceAndComments(); });
			}
			else if (LookAhead(1) == '*')
			{
				if (m_options.m_preserve_comments)
				{
					return {};
				}

				BeginToken();
				Advance(); // '/'
				Advance(); // '*'
				return SkipBlockComment()
					.and_then([this]() -> MarmotResult::VoidResult { return SkipWhitespaceAndComments(); });
			}
			return {};
		}
		default:
			return {};
		}
	}
}

MarmotResult::TokenResult Lexer::MatchString()
{
	std::string text;
	while (true)
	{
		if (IsAtEnd(0))
		{
			return std::unexpected(MarmotError::GenerateLexerErrorWithContext("Unterminated string", m_cursor.m_begin_line, BeginColumn(), m_source.m_file_name, m_source.m_lines));
		}

		if (LookAhead(0) == '"')
		{
			Advance();
			return MakeToken(Token::Name::TEXT_LITERAL, std::move(text));
		}

		if (LookAhead(0) == '\n')
		{
			AdvanceLine();
			text += '\n';
			continue;
		}

		if (LookAhead(0) != '\\' || IsAtEnd(1))
		{
			text += Advance();
			continue;
		}

		const int column = CurrentColumn();
		const auto invalid_escape = [this, column](std::string_view message) -> MarmotResult::TokenResult
		{
			return std::unexpected(MarmotError::GenerateLexerErrorWithContext(message, m_cursor.m_line, column, m_source.m_file_name, m_source.m_lines));
		};
		Advance();
		const char escape = Advance();

		switch (escape)
		{
		case 't': text += '\t'; break;
		case 'n': text += '\n'; break;
		case 'b': text += '\b'; break;
		case 'f': text += '\f'; break;
		case 'r': text += '\r'; break;
		case '0': text += '\0'; break;
		case '"': text += '"'; break;
		case '\\': text += '\\'; break;
		case 'x':
		{
			if (!IsHexDigit(LookAhead(0)) || !IsHexDigit(LookAhead(1)))
			{
				return invalid_escape("Expected two hexadecimal digits after '\\x'.");
			}
			const unsigned high = HexDigitValue(Advance());
			text += static_cast<char>((high << 4) | HexDigitValue(Advance()));
			break;
		}
		case 'u':
		{
			if (!MatchNext('{') || !IsHexDigit(LookAhead(0)))
			{
				return invalid_escape("Expected a Unicode escape of the form '\\u{...}'.");
			}
			unsigned scalar = 0;
			while (IsHexDigit(LookAhead(0)))
			{
				scalar = (scalar << 4) | HexDigitValue(Advance());
				if (scalar > 0x10FFFF)
				{
					return invalid_escape("Unicode escape is outside the Unicode scalar range.");
				}
			}
			if (!MatchNext('}'))
			{
				return invalid_escape("Expected '}' after the Unicode escape.");
			}
			if (scalar >= 0xD800 && scalar <= 0xDFFF)
			{
				return invalid_escape("Unicode escape is outside the Unicode scalar range.");
			}
			AppendUtf8(text, scalar);
			break;
		}
		default:
			return invalid_escape("Unknown string escape.");
		}
	}
}

MarmotResult::TokenResult Lexer::MatchNumber()
{
	if (IsHexPrefix())
	{
		return MatchPrefixedInteger(IsHexDigit);
	}

	if (IsBinaryPrefix())
	{
		return MatchPrefixedInteger(IsBinaryDigit);
	}

	return MatchDecimalNumber();
}

MarmotResult::TokenResult Lexer::MatchDecimalNumber()
{
	ConsumeDigits();

	const bool is_float = LookAhead(0) == '.' && IsDigit(LookAhead(1));

	if (!is_float)
	{
		return MakeTokenResult(Token::Name::INTEGER_LITERAL);
	}

	Advance();
	ConsumeDigits();
	return MakeTokenResult(Token::Name::FLOAT_LITERAL);
}

MarmotResult::TokenResult Lexer::MatchPrefixedInteger(bool (*predicate)(char))
{
	Advance();
	ConsumeWhile(predicate);
	return MakeTokenResult(Token::Name::INTEGER_LITERAL);
}

bool Lexer::IsHexPrefix() const
{
	return LookAhead(-1) == '0' && (LookAhead(0) == 'x' || LookAhead(0) == 'X');
}

bool Lexer::IsBinaryPrefix() const
{
	return LookAhead(-1) == '0' && (LookAhead(0) == 'b' || LookAhead(0) == 'B');
}

bool Lexer::IsHexDigit(char c)
{
	return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool Lexer::IsBinaryDigit(char c)
{
	return c == '0' || c == '1';
}

MarmotResult::TokenResult Lexer::MatchIdentifierOrReserved()
{
	ConsumeAlphaNumeric();

	std::string identifier = m_source.m_code.substr(m_cursor.m_begin, m_cursor.m_current - m_cursor.m_begin);

	return MakeTokenResult(Token::KeywordName(identifier).value_or(Token::Name::IDENTIFIER_LITERAL));
}

MarmotResult::TokenResult Lexer::LexTokenAfterWhitespace()
{
	return IsAtEnd(0)
		? MakeEndOfFileTokenResult()
		: BeginToken().LexTokenFrom(Advance());
}

MarmotResult::TokenResult Lexer::LexTokenFrom(char next_char)
{
	switch (next_char)
	{
	case '(':
		return MakeTokenResult(Token::Name::LEFT_PAREN);
	case ')':
		return MakeTokenResult(Token::Name::RIGHT_PAREN);
	case '{':
		return MakeTokenResult(Token::Name::LEFT_BRACE);
	case '}':
		return MakeTokenResult(Token::Name::RIGHT_BRACE);
	case '[':
		return MakeTokenResult(Token::Name::LEFT_BRACKET);
	case ']':
		return MakeTokenResult(Token::Name::RIGHT_BRACKET);
	case ',':
		return MakeTokenResult(Token::Name::COMMA);
	case '.':
		return MatchDot();
	case ';':
		return MakeTokenResult(Token::Name::SINGLE_SEMICOLON);
	case '+':
		return MatchPlus();
	case '-':
		return MatchMinus();
	case ':':
		return MatchColon();
	case '%':
		return MatchPercent();
	case '*':
		return MatchStar();
	case '/':
		return MatchSlash();
	case '|':
		return MatchPipe();
	case '^':
		return MatchCaret();
	case '&':
		return MatchAmpersand();
	case '!':
		return MatchBang();
	case '=':
		return MatchEqual();
	case '>':
		return MatchGreater();
	case '<':
		return MatchLess();
	case '~':
		return MatchTilde();
	case '#':
		return MakeTokenResult(Token::Name::HASH);
	case '"':
		return MatchString();
	default:
		return MatchLiteralOrIdentifier(next_char);
	}
}

MarmotResult::TokenResult Lexer::MatchDot()
{
	if (IsDigit(LookAhead(0)))
	{
		return MatchNumber();
	}

	return MatchNext('.')
		? MakeTokenResult(Token::Name::DOUBLE_DOT)
		: MakeTokenResult(Token::Name::SINGLE_DOT);
}

MarmotResult::TokenResult Lexer::MatchPlus()
{
	return MatchNext('+')
		? MakeTokenResult(Token::Name::DOUBLE_PLUS)
		: MakeTokenResult(Token::Name::SINGLE_PLUS);
}

MarmotResult::TokenResult Lexer::MatchMinus()
{
	if (MatchNext('>'))
	{
		return MakeTokenResult(Token::Name::THIN_ARROW);
	}

	return MakeTokenResult(Token::Name::SINGLE_MINUS);
}

MarmotResult::TokenResult Lexer::MatchColon()
{
	if (MatchNext(':'))
	{
		return MakeTokenResult(Token::Name::DOUBLE_COLON);
	}
	return MatchNext('=')
		? MakeTokenResult(Token::Name::COLON_EQUAL)
		: MakeTokenResult(Token::Name::SINGLE_COLON);
}

MarmotResult::TokenResult Lexer::MatchPercent()
{
	return MakeTokenResult(Token::Name::PERCENT);
}

MarmotResult::TokenResult Lexer::MatchStar()
{
	return MakeTokenResult(Token::Name::STAR);
}

MarmotResult::TokenResult Lexer::MatchSlash()
{
	if (m_options.m_preserve_comments && LookAhead(0) == '/')
	{
		return MatchLineComment();
	}

	if (m_options.m_preserve_comments && LookAhead(0) == '*')
	{
		return MatchBlockComment();
	}

	return MakeTokenResult(Token::Name::SLASH);
}

MarmotResult::TokenResult Lexer::MatchLineComment()
{
	Advance();
	ConsumeWhile([](char c) { return c != '\n' && c != '\r'; });
	return MakeTokenResult(Token::Name::LINE_COMMENT);
}

MarmotResult::TokenResult Lexer::MatchBlockComment()
{
	Advance();

	while (true)
	{
		if (LookAhead(0) == '*' && LookAhead(1) == '/')
		{
			Advance();
			Advance();
			return MakeTokenResult(Token::Name::BLOCK_COMMENT);
		}

		if (IsAtEnd(0))
		{
			return std::unexpected(MarmotError::GenerateLexerErrorWithContext("Unterminated block comment", m_cursor.m_begin_line, BeginColumn(), m_source.m_file_name, m_source.m_lines));
		}

		if (LookAhead(0) == '\n')
		{
			AdvanceLine();
			continue;
		}

		Advance();
	}
}

MarmotResult::TokenResult Lexer::MatchPipe()
{
	if (MatchNext('|'))
	{
		return MakeTokenResult(Token::Name::DOUBLE_BAR);
	}

	if (MatchNext('>'))
	{
		return MakeTokenResult(Token::Name::BAR_BRACKET);
	}

	return MakeTokenResult(Token::Name::SINGLE_BAR);
}

MarmotResult::TokenResult Lexer::MatchCaret()
{
	return MakeTokenResult(Token::Name::CARET);
}

MarmotResult::TokenResult Lexer::MatchAmpersand()
{
	if (MatchNext('&'))
	{
		return MakeTokenResult(Token::Name::DOUBLE_AMPERSAND);
	}

	return MakeTokenResult(Token::Name::SINGLE_AMPERSAND);
}

MarmotResult::TokenResult Lexer::MatchBang()
{
	return MatchNext('=')
		? MakeTokenResult(Token::Name::BANG_EQUAL)
		: MakeTokenResult(Token::Name::BANG);
}

MarmotResult::TokenResult Lexer::MatchEqual()
{
	if (MatchNext('='))
	{
		return MakeTokenResult(Token::Name::DOUBLE_EQUAL);
	}

	if (MatchNext('>'))
	{
		return MakeTokenResult(Token::Name::FAT_ARROW);
	}

	return MakeTokenResult(Token::Name::SINGLE_EQUAL);
}

MarmotResult::TokenResult Lexer::MatchGreater()
{
	if (MatchNext('>'))
	{
		return MatchRightShift();
	}

	return MatchNext('=')
		? MakeTokenResult(Token::Name::GREATER_EQUAL)
		: MakeTokenResult(Token::Name::RIGHT_ANGLE);
}

MarmotResult::TokenResult Lexer::MatchLess()
{
	if (MatchNext('<'))
	{
		return MatchLeftShift();
	}

	if (MatchNext('='))
	{
		return MakeTokenResult(Token::Name::LESS_EQUAL);
	}

	return MakeTokenResult(Token::Name::LEFT_ANGLE);
}

MarmotResult::TokenResult Lexer::MatchLeftShift()
{
	return MakeTokenResult(Token::Name::LEFT_SHIFT);
}

MarmotResult::TokenResult Lexer::MatchTilde()
{
	return MakeTokenResult(Token::Name::TILDE);
}

MarmotResult::TokenResult Lexer::MatchRightShift()
{
	return MakeTokenResult(Token::Name::RIGHT_SHIFT);
}

MarmotResult::TokenResult Lexer::MatchLiteralOrIdentifier(char next_char)
{
	if (IsDigit(next_char))
	{
		return MatchNumber();
	}

	if (IsAlpha(next_char))
	{
		return MatchIdentifierOrReserved();
	}

	if (next_char == '\0')
	{
		return MakeEndOfFileTokenResult();
	}

	return MakeInvalidCharacterError(next_char);
}

MarmotResult::TokenResult Lexer::MakeInvalidCharacterError(char next_char) const
{
	return std::unexpected(MarmotError::GenerateLexerErrorWithContext("Invalid character: "s + next_char, m_cursor.m_line, BeginColumn(), m_source.m_file_name, m_source.m_lines));
}

MarmotResult::Result<Lexer::LexState> Lexer::RecordTokenOrError(LexState state)
{
	return LexOneToken()
		.transform
		(
			[state = std::move(state), this](Token&& token) mutable -> LexState
			{
				if (token.m_token_name != Token::Name::END_OF_FILE)
				{
					m_cursor.m_last_token_end_line = m_cursor.m_line;
					m_cursor.m_last_token_end_column = CurrentColumn();
					m_cursor.m_has_last_token_end = true;
				}

				state.m_tokens.AddToken(std::move(token));
				return std::move(state);
			}
		);
}

MarmotResult::TokenResult Lexer::LexOneToken()
{
	return SkipWhitespaceAndComments()
		.and_then([this]() -> MarmotResult::TokenResult { return LexTokenAfterWhitespace(); });
}

std::vector<std::string> Lexer::SplitIntoLines(const std::string& source)
{
	std::istringstream iss(source);
	std::vector<std::string> lines;
	std::string line;

	while (std::getline(iss, line))
	{
		lines.emplace_back(std::move(line));
	}

	return lines;
}

Lexer::Source Lexer::BuildSource(std::string&& source_code, std::string_view file_name)
{
	MarmotSource::RemoveByteOrderMark(source_code);
	std::vector<std::string> lines = SplitIntoLines(source_code);
	return Source{ std::move(source_code), std::string(file_name), std::move(lines) };
}

Lexer::Lexer(std::string&& source_code, std::string_view file_name) noexcept
	: Lexer(std::move(source_code), file_name, Options{})
{
}

Lexer::Lexer(std::string&& source_code, std::string_view file_name, Options options) noexcept
	: m_source(BuildSource(std::move(source_code), file_name)),
	m_options(options)
{
}

MarmotResult::LexerResult Lexer::LexRecursive(LexState state)
{
	while (!IsAtEnd(0))
	{
		MarmotResult::VoidResult step = RecordTokenOrError(std::move(state))
			.and_then
			(
				[&state](LexState&& next_state) -> MarmotResult::VoidResult
				{
					state = std::move(next_state);
					return {};
				}
			);
		if (!step)
		{
			return std::unexpected(step.error());
		}
	}

	if (state.m_tokens.Size() == 0 || (std::prev(state.m_tokens.cend())->m_token_name != Token::Name::END_OF_FILE))
	{
		BeginToken();
		state.m_tokens.AddToken(MakeEndOfFileToken());
	}

	return std::move(state.m_tokens);
}

MarmotResult::LexerResult Lexer::Lex() &
{
	return LexRecursive(LexState{ TokenStream{} });
}

MarmotResult::LexerResult Lexer::Lex() &&
{
	return LexRecursive(LexState{ TokenStream{} });
}

