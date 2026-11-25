#include "Token.h"

#include <array>
#include <queue>
#include <map>
#include <unordered_map>

namespace
{
	struct TokenDefinition
	{
		Token::Name m_name;
		std::string_view m_spelling;
		bool m_is_keyword;
	};

	constexpr std::array s_definitions
	{
#define MARMOT_TOKEN(name, spelling, is_keyword) TokenDefinition{ Token::Name::name, spelling, is_keyword },
#include "Compiler/Token/Tokens.def"
#undef MARMOT_TOKEN
	};
}

std::string_view Token::Spelling(Name token_name)
{
	return s_definitions[static_cast<size_t>(token_name)].m_spelling;
}

std::optional<Token::Name> Token::KeywordName(std::string_view spelling)
{
	static const std::unordered_map<std::string_view, Name> s_keywords = []
	{
		std::unordered_map<std::string_view, Name> keywords;
		for (const TokenDefinition& definition : s_definitions)
		{
			if (definition.m_is_keyword)
			{
				keywords.emplace(definition.m_spelling, definition.m_name);
			}
		}
		return keywords;
	}();
	const std::unordered_map<std::string_view, Name>::const_iterator keyword = s_keywords.find(spelling);
	return keyword == s_keywords.cend() ? std::nullopt : std::optional<Name>(keyword->second);
}

bool Token::IsKeyword(Name token_name)
{
	return s_definitions[static_cast<size_t>(token_name)].m_is_keyword;
}

Token::Token(std::string lexeme, Name token_name, int line, std::string_view file_name) noexcept
	: m_file_name(file_name),
	m_lexeme(std::move(lexeme)),
	m_token_name(token_name),
	m_line(line)
{
}

Token::Token(std::string lexeme, Name token_name, int line, std::string_view file_name, int column, size_t source_length) noexcept
	: m_file_name(file_name),
	m_lexeme(std::move(lexeme)),
	m_token_name(token_name),
	m_line(line),
	m_column(column),
	m_source_length(source_length)
{
}

Token::Token(std::string lexeme, Name token_name, const Token& anchor) noexcept
	: m_file_name(anchor.m_file_name),
	m_lexeme(std::move(lexeme)),
	m_token_name(token_name),
	m_line(anchor.m_line),
	m_column(anchor.m_column),
	m_source_length(anchor.m_source_length)
{
}

Token::Token(Name token_name, const Token& anchor) noexcept
	: Token(std::string(Spelling(token_name)), token_name, anchor)
{
}

TokenStream::TokenStream(const TokenStream& other)
	: m_doc_comment_lines(other.m_doc_comment_lines)
{
	m_tokens.reserve(other.m_tokens.size());
	for (const std::unique_ptr<Token>& token : other.m_tokens)
	{
		m_tokens.emplace_back(std::make_unique<Token>(*token));
	}
}

TokenStream& TokenStream::operator=(const TokenStream& other)
{
	if (this != &other)
	{
		Storage copied;
		copied.reserve(other.m_tokens.size());
		for (const std::unique_ptr<Token>& token : other.m_tokens)
		{
			copied.emplace_back(std::make_unique<Token>(*token));
		}
		m_tokens = std::move(copied);
		m_doc_comment_lines = other.m_doc_comment_lines;
	}
	return *this;
}

TokenStream::iterator TokenStream::begin()
{ 
	return TokenStream::iterator(m_tokens.begin());
}

TokenStream::iterator TokenStream::end()
{ 
	return TokenStream::iterator(m_tokens.end());
}

TokenStream::const_iterator TokenStream::cbegin() const
{ 
	return TokenStream::const_iterator(m_tokens.cbegin());
}

TokenStream::const_iterator TokenStream::cend() const
{ 
	return TokenStream::const_iterator(m_tokens.cend());
}

TokenStream& TokenStream::AddToken(Token&& token)
{ 
	m_tokens.emplace_back(std::make_unique<Token>(std::move(token)));
	return *this;
}

Token& TokenStream::operator[](int index) const 
{ 
	return *m_tokens[static_cast<size_t>(index)];
}

int TokenStream::Size() const 
{ 
	return static_cast<int>(m_tokens.size()); 
}

void TokenStream::SetDocCommentLines(std::vector<int> lines)
{
	m_doc_comment_lines = std::move(lines);
}

std::vector<int> TokenStream::TakeDocCommentLines() &&
{
	return std::move(m_doc_comment_lines);
}

void TokenStream::Insert(TokenStream::iterator iter, TokenStream&& tokens)
{
	m_tokens.insert(iter.Position(), std::make_move_iterator(tokens.m_tokens.begin()), std::make_move_iterator(tokens.m_tokens.end()));
}

void TokenStream::Erase(TokenStream::iterator first, TokenStream::iterator last)
{
	m_tokens.erase(first.Position(), last.Position());
}
