#include "Lexer.h"

#include "Compiler/Source/Source.h"

#include <algorithm>
#include <ranges>

namespace
{
	using TokenName = Token::Name;
	using MarmotCompiler::Failure;
	using namespace std::string_view_literals;

	struct Lexeme
	{
		TokenName m_name;
		size_t m_length;
		std::string m_decoded_text{};
	};

	struct ScanError
	{
		std::string m_message;
		size_t m_offset = 0;
	};

	using ScanResult = MarmotCompiler::Expected<Lexeme, ScanError>;

	bool IsDigit(char c)
	{
		return c >= '0' && c <= '9';
	}

	bool IsAlpha(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
	}

	bool IsAlphaNumeric(char c)
	{
		return IsAlpha(c) || IsDigit(c);
	}

	bool IsHexDigit(char c)
	{
		return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
	}

	bool IsBinaryDigit(char c)
	{
		return c == '0' || c == '1';
	}

	bool IsWhitespace(char c)
	{
		return " \t\r\n"sv.contains(c);
	}

	template<typename Predicate>
	size_t ScanWhile(std::string_view source, size_t offset, Predicate predicate)
	{
		while (offset < source.size() && predicate(source[offset]))
		{
			offset += 1;
		}
		return offset;
	}

	Lexeme ScanNumber(std::string_view source)
	{
		if (source.starts_with("0x") || source.starts_with("0X"))
		{
			return { TokenName::INTEGER_LITERAL, ScanWhile(source, 2, IsHexDigit) };
		}
		if (source.starts_with("0b") || source.starts_with("0B"))
		{
			return { TokenName::INTEGER_LITERAL, ScanWhile(source, 2, IsBinaryDigit) };
		}

		const size_t digits_end = ScanWhile(source, 1, IsDigit);
		if (digits_end + 1 < source.size() && source[digits_end] == '.' && IsDigit(source[digits_end + 1]))
		{
			return { TokenName::FLOAT_LITERAL, ScanWhile(source, digits_end + 2, IsDigit) };
		}
		return { TokenName::INTEGER_LITERAL, digits_end };
	}

	unsigned HexDigitValue(char digit)
	{
		if (IsDigit(digit))
		{
			return static_cast<unsigned>(digit - '0');
		}
		return static_cast<unsigned>((digit | 0x20) - 'a' + 10);
	}

	std::string EncodeUtf8(unsigned scalar)
	{
		std::string text;
		const auto append = [&text](unsigned byte) { text += static_cast<char>(byte); };
		if (scalar <= 0x7F)
		{
			append(scalar);
			return text;
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
		return text;
	}

	struct Escape
	{
		std::string m_text;
		size_t m_length;
	};

	using EscapeResult = MarmotCompiler::Expected<Escape, std::string_view>;

	EscapeResult ScanUnicodeEscape(std::string_view source)
	{
		if (source.size() < 4 || source[2] != '{' || !IsHexDigit(source[3]))
		{
			return Failure("Expected a Unicode escape of the form '\\u{...}'."sv);
		}

		const std::string_view invalid_scalar = "Unicode escape is outside the Unicode scalar range.";
		size_t offset = 3;
		unsigned scalar = 0;
		while (offset < source.size() && IsHexDigit(source[offset]))
		{
			scalar = (scalar << 4) | HexDigitValue(source[offset]);
			if (scalar > 0x10FFFF)
			{
				return Failure(invalid_scalar);
			}
			offset += 1;
		}
		if (offset == source.size() || source[offset] != '}')
		{
			return Failure("Expected '}' after the Unicode escape."sv);
		}
		if (scalar >= 0xD800 && scalar <= 0xDFFF)
		{
			return Failure(invalid_scalar);
		}
		return Escape{ EncodeUtf8(scalar), offset + 1 };
	}

	EscapeResult ScanEscape(std::string_view source)
	{
		switch (source[1])
		{
		case 't': return Escape{ "\t", 2 };
		case 'n': return Escape{ "\n", 2 };
		case 'b': return Escape{ "\b", 2 };
		case 'f': return Escape{ "\f", 2 };
		case 'r': return Escape{ "\r", 2 };
		case '0': return Escape{ std::string(1, '\0'), 2 };
		case '"': return Escape{ "\"", 2 };
		case '\\': return Escape{ "\\", 2 };
		case 'x':
		{
			if (source.size() < 4 || !IsHexDigit(source[2]) || !IsHexDigit(source[3]))
			{
				return Failure("Expected two hexadecimal digits after '\\x'."sv);
			}
			const unsigned byte = (HexDigitValue(source[2]) << 4) | HexDigitValue(source[3]);
			return Escape{ std::string(1, static_cast<char>(byte)), 4 };
		}
		case 'u':
			return ScanUnicodeEscape(source);
		default:
			return Failure("Unknown string escape."sv);
		}
	}

	ScanResult ScanString(std::string_view source)
	{
		std::string text;
		size_t offset = 1;
		while (offset < source.size())
		{
			const char c = source[offset];
			if (c == '"')
			{
				return Lexeme{ TokenName::TEXT_LITERAL, offset + 1, std::move(text) };
			}
			if (c != '\\' || offset + 1 == source.size())
			{
				text += c;
				offset += 1;
				continue;
			}

			const EscapeResult escape = ScanEscape(source.substr(offset));
			if (!escape)
			{
				return Failure(ScanError{ std::string(escape.Error()), offset });
			}
			text += escape->m_text;
			offset += escape->m_length;
		}
		return Failure(ScanError{ "Unterminated string" });
	}

	MarmotCompiler::Optional<TokenName> ScanSymbol(char first, char second)
	{
		switch (first)
		{
		case '(': return TokenName::LEFT_PAREN;
		case ')': return TokenName::RIGHT_PAREN;
		case '{': return TokenName::LEFT_BRACE;
		case '}': return TokenName::RIGHT_BRACE;
		case '[': return TokenName::LEFT_BRACKET;
		case ']': return TokenName::RIGHT_BRACKET;
		case ',': return TokenName::COMMA;
		case ';': return TokenName::SINGLE_SEMICOLON;
		case '%': return TokenName::PERCENT;
		case '*': return TokenName::STAR;
		case '/': return TokenName::SLASH;
		case '^': return TokenName::CARET;
		case '~': return TokenName::TILDE;
		case '#': return TokenName::HASH;
		case '.': return second == '.' ? TokenName::DOUBLE_DOT : TokenName::SINGLE_DOT;
		case '+': return second == '+' ? TokenName::DOUBLE_PLUS : TokenName::SINGLE_PLUS;
		case '-': return second == '>' ? TokenName::THIN_ARROW : TokenName::SINGLE_MINUS;
		case '&': return second == '&' ? TokenName::DOUBLE_AMPERSAND : TokenName::SINGLE_AMPERSAND;
		case '!': return second == '=' ? TokenName::BANG_EQUAL : TokenName::BANG;
		case ':':
			if (second == ':')
			{
				return TokenName::DOUBLE_COLON;
			}
			return second == '=' ? TokenName::COLON_EQUAL : TokenName::SINGLE_COLON;
		case '|':
			if (second == '|')
			{
				return TokenName::DOUBLE_BAR;
			}
			return second == '>' ? TokenName::BAR_BRACKET : TokenName::SINGLE_BAR;
		case '=':
			if (second == '=')
			{
				return TokenName::DOUBLE_EQUAL;
			}
			return second == '>' ? TokenName::FAT_ARROW : TokenName::SINGLE_EQUAL;
		case '>':
			if (second == '>')
			{
				return TokenName::RIGHT_SHIFT;
			}
			return second == '=' ? TokenName::GREATER_EQUAL : TokenName::RIGHT_ANGLE;
		case '<':
			if (second == '<')
			{
				return TokenName::LEFT_SHIFT;
			}
			return second == '=' ? TokenName::LESS_EQUAL : TokenName::LEFT_ANGLE;
		default:
			return MarmotCompiler::None;
		}
	}

	ScanResult ScanLexeme(std::string_view source, bool preserve_comments)
	{
		const char first = source.front();
		const char second = source.size() > 1 ? source[1] : '\0';
		if (first == '\0')
		{
			// A NUL byte emits an EOF token without discarding the rest of the source.
			return Lexeme{ TokenName::END_OF_FILE, 1 };
		}
		if (IsWhitespace(first))
		{
			return Lexeme{ TokenName::WHITESPACE, ScanWhile(source, 1, IsWhitespace) };
		}
		if (IsAlpha(first))
		{
			const size_t end = ScanWhile(source, 1, IsAlphaNumeric);
			const TokenName name = Token::KeywordName(source.substr(0, end)).ValueOr(TokenName::IDENTIFIER_LITERAL);
			return Lexeme{ name, end };
		}
		if (IsDigit(first) || (first == '.' && IsDigit(second)))
		{
			return ScanNumber(source);
		}
		if (first == '"')
		{
			return ScanString(source);
		}
		if (first == '/' && second == '/')
		{
			const size_t end = source.find_first_of(preserve_comments ? "\r\n" : "\n", 2);
			return Lexeme{ TokenName::LINE_COMMENT, std::min(end, source.size()) };
		}
		if (first == '/' && second == '*')
		{
			const size_t close = source.find("*/", 2);
			if (close == std::string_view::npos)
			{
				return Failure(ScanError{ "Unterminated block comment" });
			}
			return Lexeme{ TokenName::BLOCK_COMMENT, close + 2 };
		}
		if (const MarmotCompiler::Optional<TokenName> name = ScanSymbol(first, second))
		{
			return Lexeme{ *name, Token::Spelling(*name).size() };
		}
		return Failure(ScanError{ std::string("Invalid character: ") + first });
	}

	MarmotSource::Position PositionAfter(MarmotSource::Position position, std::string_view consumed)
	{
		size_t line_start = 0;
		for (size_t newline = consumed.find('\n'); newline != std::string_view::npos; newline = consumed.find('\n', line_start))
		{
			position.m_line += 1;
			position.m_column = 0;
			line_start = newline + 1;
		}
		position.m_column += consumed.size() - line_start;
		return position;
	}

	Token MakeEndOfFileToken(MarmotSource::Position anchor, std::string_view file_name)
	{
		return Token({}, TokenName::END_OF_FILE, anchor.m_line, file_name, static_cast<int>(anchor.m_column), 0);
	}

	std::vector<std::string> SplitIntoLines(std::string_view source)
	{
		auto lines = source | std::views::split('\n') | std::ranges::to<std::vector<std::string>>();
		if (source.ends_with('\n'))
		{
			lines.pop_back();
		}
		return lines;
	}
}

Lexer::Lexer(std::string&& source_code, std::string_view file_name) noexcept
	: Lexer(std::move(source_code), file_name, Options{})
{
}

Lexer::Lexer(std::string&& source_code, std::string_view file_name, Options options) noexcept
	: m_source_code(std::move(source_code)),
	m_file_name(file_name),
	m_options(options)
{
	MarmotSource::RemoveByteOrderMark(m_source_code);
}

MarmotResult::LexerResult Lexer::Lex() const
{
	const std::string_view source = m_source_code;
	std::string_view remaining = source;
	MarmotSource::Position position{ 1, 0 };
	MarmotCompiler::Optional<MarmotSource::Position> last_token_end;
	TokenStream tokens;
	std::vector<int> doc_comment_lines;

	while (!remaining.empty())
	{
		ScanResult scanned = ScanLexeme(remaining, m_options.m_preserve_comments);
		if (!scanned)
		{
			const ScanError& error = scanned.Error();
			const MarmotSource::Position at = PositionAfter(position, remaining.substr(0, error.m_offset));
			return Failure(MarmotError::GenerateLexerErrorWithContext(
				error.m_message, at.m_line, static_cast<int>(at.m_column), m_file_name, SplitIntoLines(source)));
		}

		Lexeme& lexeme = *scanned;
		const std::string_view spelling = remaining.substr(0, lexeme.m_length);
		const MarmotSource::Position end = PositionAfter(position, spelling);
		if (lexeme.m_name == TokenName::END_OF_FILE)
		{
			// The final EOF token also covers a NUL at the end of the source.
			if (lexeme.m_length < remaining.size())
			{
				tokens.AddToken(MakeEndOfFileToken(last_token_end.ValueOr(end), m_file_name));
			}
		}
		else if (!m_options.m_preserve_comments && lexeme.m_name == TokenName::LINE_COMMENT)
		{
			const size_t line_start = source.size() - remaining.size() - position.m_column;
			if (spelling.starts_with("///") &&
				source.substr(line_start, position.m_column).find_first_not_of(" \t\r") == std::string_view::npos)
			{
				doc_comment_lines.push_back(position.m_line);
			}
		}
		else if (lexeme.m_name != TokenName::WHITESPACE &&
			(m_options.m_preserve_comments || lexeme.m_name != TokenName::BLOCK_COMMENT))
		{
			std::string text = lexeme.m_name == TokenName::TEXT_LITERAL ? std::move(lexeme.m_decoded_text) : std::string(spelling);
			tokens.AddToken(Token(std::move(text), lexeme.m_name, position.m_line, m_file_name, static_cast<int>(position.m_column), spelling.size()));
			last_token_end = end;
		}

		remaining.remove_prefix(lexeme.m_length);
		position = end;
	}

	tokens.AddToken(MakeEndOfFileToken(last_token_end.ValueOr(position), m_file_name));
	tokens.SetDocCommentLines(std::move(doc_comment_lines));
	return tokens;
}
