#pragma once

#include <compare>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Compiler/Module/Module.h"

struct Token
{
	enum class Name
	{
		// symbol
		THIN_ARROW,
		LEFT_ARROW,
		FAT_ARROW,
		LEFT_PAREN,
		RIGHT_PAREN,
		LEFT_BRACE,
		RIGHT_BRACE,
		LEFT_BRACKET,
		RIGHT_BRACKET,
		COMMA,
		SINGLE_DOT,
		DOUBLE_DOT,
		SINGLE_SEMICOLON,
		SINGLE_PLUS,
		DOUBLE_PLUS,
		SINGLE_MINUS,
		DOUBLE_MINUS,
		LEFT_SHIFT,
		RIGHT_SHIFT,
		PERCENT,
		STAR,
		SLASH,
		SINGLE_BAR,
		DOUBLE_BAR,
		BAR_BRACKET,
		CARET,
		SINGLE_AMPERSAND,
		DOUBLE_AMPERSAND,
		BANG,
		BANG_EQUAL,
		SINGLE_EQUAL,
		DOUBLE_EQUAL,
		RIGHT_ANGLE,
		GREATER_EQUAL,
		LEFT_ANGLE,
		LESS_EQUAL,
		SINGLE_COLON,
		DOUBLE_COLON,
		TILDE,
		HASH,
		PLUS_PLUS_EQUAL,
		EQUAL_PLUS_PLUS,
		PLUS_EQUAL,
		MINUS_EQUAL,
		STAR_EQUAL,
		SLASH_EQUAL,
		PERCENT_EQUAL,
		AMPERSAND_EQUAL,
		BAR_EQUAL,
		CARET_EQUAL,
		LEFT_SHIFT_EQUAL,
		RIGHT_SHIFT_EQUAL,

		// Literal
		IDENTIFIER_LITERAL,
		TEXT_LITERAL,
		FLOAT_LITERAL,
		INTEGER_LITERAL,

		// reserved
		ELSE,
		FALSE,
		FUNCTION,
		FOR,
		IN,
		IF,
		TRUE,
		DEF,
		IMPORT,
		AS,
		FOREIGN,
		CASE,
		MATCH,
		THEN,
		WITH,
		MODULE,
		EXPORT,
		PUBLIC,
		PRIVATE,
		USE,
		CLASS,
		INSTANCE,
		WHERE,
		TYPE,
		// Must stay inside this reserved block: ModuleManager::IsKeyword tests
		// `>= Token::Name::ELSE` ordinally, so a keyword placed outside it stops
		// being rejected as a module name, silently and with no test to catch it.
		ALIAS,
		DERIVING,

		// types
		FLOAT,
		INTEGER,
		BYTE,
		WORD,
		TEXT,
		BOOL,
		UNIT,
		ARRAY,
		NEVER,
		RANGE,
		WORKER,
		CHANNEL_TYPE,

		WHITESPACE,
		LINE_COMMENT,
		BLOCK_COMMENT,
		END_OF_FILE,
	};

	std::string m_file_name;
	std::string m_lexeme;
	Name m_token_name;
	int m_line;
	std::optional<int> m_column = std::nullopt;
	std::optional<size_t> m_source_length = std::nullopt;

	Token(std::string lexeme, Name token_name, int line, std::string_view file_name) noexcept;
	Token(std::string lexeme, Name token_name, int line, std::string_view file_name, int column, size_t source_length) noexcept;
	Token(std::string lexeme, Name token_name, const Token& anchor) noexcept;
};

// Tokens are held indirectly so that every Token keeps a stable address for the
// life of the stream. The parser holds Token& into the stream across whole
// sub-parses, and splitting a '>>' that closes two generic levels inserts into
// the stream mid-parse; storing Tokens by value would reallocate the buffer and
// dangle every one of those references.
class TokenStream
{
private:
	using Storage = std::vector<std::unique_ptr<Token>>;

	template<typename StorageIterator, typename TokenReference, typename TokenPointer>
	class BasicIterator
	{
	public:
		using iterator_category = std::random_access_iterator_tag;
		using iterator_concept = std::random_access_iterator_tag;
		using value_type = Token;
		using difference_type = std::ptrdiff_t;
		using reference = TokenReference;
		using pointer = TokenPointer;

		BasicIterator() = default;

		explicit BasicIterator(StorageIterator position) noexcept
			: m_position(position)
		{
		}

		StorageIterator Position() const noexcept
		{
			return m_position;
		}

		reference operator*() const
		{
			return **m_position;
		}

		pointer operator->() const
		{
			return m_position->get();
		}

		reference operator[](difference_type offset) const
		{
			return *m_position[offset];
		}

		BasicIterator& operator++() noexcept
		{
			m_position += 1;
			return *this;
		}

		BasicIterator operator++(int) noexcept
		{
			BasicIterator previous = *this;
			m_position += 1;
			return previous;
		}

		BasicIterator& operator--() noexcept
		{
			m_position -= 1;
			return *this;
		}

		BasicIterator operator--(int) noexcept
		{
			BasicIterator previous = *this;
			m_position -= 1;
			return previous;
		}

		BasicIterator& operator+=(difference_type offset) noexcept
		{
			m_position += offset;
			return *this;
		}

		BasicIterator& operator-=(difference_type offset) noexcept
		{
			m_position -= offset;
			return *this;
		}

		friend BasicIterator operator+(BasicIterator iterator, difference_type offset) noexcept
		{
			return BasicIterator(iterator.m_position + offset);
		}

		friend BasicIterator operator+(difference_type offset, BasicIterator iterator) noexcept
		{
			return BasicIterator(iterator.m_position + offset);
		}

		friend BasicIterator operator-(BasicIterator iterator, difference_type offset) noexcept
		{
			return BasicIterator(iterator.m_position - offset);
		}

		friend difference_type operator-(const BasicIterator& left, const BasicIterator& right) noexcept
		{
			return left.m_position - right.m_position;
		}

		friend bool operator==(const BasicIterator& left, const BasicIterator& right) noexcept
		{
			return left.m_position == right.m_position;
		}

		friend std::strong_ordering operator<=>(const BasicIterator& left, const BasicIterator& right) noexcept
		{
			return left.m_position <=> right.m_position;
		}

	private:
		StorageIterator m_position{};
	};

	Storage m_tokens;

public:
	using iterator = BasicIterator<Storage::iterator, Token&, Token*>;
	using const_iterator = BasicIterator<Storage::const_iterator, const Token&, const Token*>;

	TokenStream() = default;

	TokenStream(const TokenStream& other);

	TokenStream& operator=(const TokenStream& other);

	TokenStream(TokenStream&& other) noexcept = default;

	TokenStream& operator=(TokenStream&& other) noexcept = default;

	iterator begin();

	iterator end();

	const_iterator cbegin() const;

	const_iterator cend() const;

	TokenStream& AddToken(Token&& token);

	Token& operator[](int index) const;

	int Size() const;

	void Insert(iterator iter, TokenStream&& tokens);

	void Erase(iterator iter);

	void PopBack() noexcept;
};

