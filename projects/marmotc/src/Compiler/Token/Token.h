#pragma once

#include <compare>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


struct Token
{
	enum class Name
	{
#define MARMOT_TOKEN(name, spelling, is_keyword) name,
#include "Compiler/Token/Tokens.def"
#undef MARMOT_TOKEN
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
	Token(Name token_name, const Token& anchor) noexcept;

	// Empty for tokens whose spelling comes from source.
	static std::string_view Spelling(Name token_name);
	static std::optional<Name> KeywordName(std::string_view spelling);
	static bool IsKeyword(Name token_name);
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

	void Erase(iterator first, iterator last);

};

