#include "Token.h"
#include "Common/Printer/Printer.h"

#include <queue>
#include <map>

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

TokenStream::TokenStream(const TokenStream& other)
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

void TokenStream::Insert(TokenStream::iterator iter, TokenStream&& tokens)
{
	m_tokens.insert(iter.Position(), std::make_move_iterator(tokens.m_tokens.begin()), std::make_move_iterator(tokens.m_tokens.end()));
}

void TokenStream::Erase(TokenStream::iterator iter)
{
	m_tokens.erase(iter.Position());
}

void TokenStream::PopBack() noexcept
{
    m_tokens.pop_back();
}

