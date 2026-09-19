#include "Common/Json/JsonReader.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>

namespace
{
	using MidoriJson::JsonValue;

	// Deep enough for any plan or report; bounded so hostile input cannot
	// exhaust the stack.
	constexpr int MAX_DEPTH = 128;

	class JsonParser
	{
	private:
		std::string_view m_text;
		size_t m_position = 0uz;

	public:
		explicit JsonParser(std::string_view text) : m_text(text) {}

		std::expected<JsonValue, std::string> ParseDocument()
		{
			std::expected<JsonValue, std::string> value = ParseValue(0);
			if (!value.has_value())
			{
				return value;
			}

			SkipWhitespace();
			if (m_position != m_text.size())
			{
				return Fail("unexpected text after the JSON value");
			}

			return value;
		}

	private:
		std::unexpected<std::string> Fail(std::string_view message) const
		{
			int line = 1;
			int column = 1;
			for (size_t index = 0uz; index < m_position && index < m_text.size(); index += 1uz)
			{
				if (m_text[index] == '\n')
				{
					line += 1;
					column = 1;
				}
				else
				{
					column += 1;
				}
			}

			return std::unexpected(std::format("line {}, column {}: {}", line, column, message));
		}

		void SkipWhitespace()
		{
			while (m_position < m_text.size())
			{
				const char ch = m_text[m_position];
				if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
				{
					return;
				}
				m_position += 1uz;
			}
		}

		bool Consume(std::string_view literal)
		{
			if (m_text.substr(m_position, literal.size()) == literal)
			{
				m_position += literal.size();
				return true;
			}
			return false;
		}

		std::expected<JsonValue, std::string> ParseValue(int depth)
		{
			if (depth > MAX_DEPTH)
			{
				return Fail("nested too deeply");
			}

			SkipWhitespace();
			if (m_position >= m_text.size())
			{
				return Fail("expected a value, found the end of the input");
			}

			const char ch = m_text[m_position];
			if (ch == '{')
			{
				return ParseObject(depth);
			}
			if (ch == '[')
			{
				return ParseArray(depth);
			}
			if (ch == '"')
			{
				std::expected<std::string, std::string> text = ParseString();
				if (!text.has_value())
				{
					return std::unexpected(text.error());
				}
				return JsonValue(std::move(text.value()));
			}
			if (Consume("true"))
			{
				return JsonValue(true);
			}
			if (Consume("false"))
			{
				return JsonValue(false);
			}
			if (Consume("null"))
			{
				return JsonValue();
			}
			if (ch == '-' || (ch >= '0' && ch <= '9'))
			{
				return ParseNumber();
			}

			return Fail(std::format("unexpected character '{}'", ch));
		}

		std::expected<JsonValue, std::string> ParseObject(int depth)
		{
			m_position += 1uz;
			JsonValue::Object members;

			SkipWhitespace();
			if (Consume("}"))
			{
				return JsonValue(std::move(members));
			}

			while (true)
			{
				SkipWhitespace();
				if (m_position >= m_text.size() || m_text[m_position] != '"')
				{
					return Fail("expected a member name in double quotes");
				}

				const size_t key_position = m_position;
				std::expected<std::string, std::string> key = ParseString();
				if (!key.has_value())
				{
					return std::unexpected(key.error());
				}

				for (const std::pair<std::string, JsonValue>& member : members)
				{
					if (member.first == key.value())
					{
						m_position = key_position;
						return Fail(std::format("duplicate member \"{}\"", key.value()));
					}
				}

				SkipWhitespace();
				if (!Consume(":"))
				{
					return Fail("expected ':' after the member name");
				}

				std::expected<JsonValue, std::string> value = ParseValue(depth + 1);
				if (!value.has_value())
				{
					return value;
				}
				members.emplace_back(std::move(key.value()), std::move(value.value()));

				SkipWhitespace();
				if (Consume(","))
				{
					continue;
				}
				if (Consume("}"))
				{
					return JsonValue(std::move(members));
				}
				return Fail("expected ',' or '}' in the object");
			}
		}

		std::expected<JsonValue, std::string> ParseArray(int depth)
		{
			m_position += 1uz;
			JsonValue::Array elements;

			SkipWhitespace();
			if (Consume("]"))
			{
				return JsonValue(std::move(elements));
			}

			while (true)
			{
				std::expected<JsonValue, std::string> value = ParseValue(depth + 1);
				if (!value.has_value())
				{
					return value;
				}
				elements.emplace_back(std::move(value.value()));

				SkipWhitespace();
				if (Consume(","))
				{
					continue;
				}
				if (Consume("]"))
				{
					return JsonValue(std::move(elements));
				}
				return Fail("expected ',' or ']' in the array");
			}
		}

		std::expected<JsonValue, std::string> ParseNumber()
		{
			// Validate the RFC 8259 grammar first; strtod alone accepts forms JSON
			// does not, such as leading zeros, hex, "inf" and "nan".
			const size_t start = m_position;
			if (m_text[m_position] == '-')
			{
				m_position += 1uz;
			}

			if (m_position < m_text.size() && m_text[m_position] == '0')
			{
				m_position += 1uz;
			}
			else if (m_position < m_text.size() && m_text[m_position] >= '1' && m_text[m_position] <= '9')
			{
				while (m_position < m_text.size() && m_text[m_position] >= '0' && m_text[m_position] <= '9')
				{
					m_position += 1uz;
				}
			}
			else
			{
				return Fail("expected a digit");
			}

			if (m_position < m_text.size() && m_text[m_position] == '.')
			{
				m_position += 1uz;
				const size_t digits = m_position;
				while (m_position < m_text.size() && m_text[m_position] >= '0' && m_text[m_position] <= '9')
				{
					m_position += 1uz;
				}
				if (m_position == digits)
				{
					return Fail("expected a digit after '.'");
				}
			}

			if (m_position < m_text.size() && (m_text[m_position] == 'e' || m_text[m_position] == 'E'))
			{
				m_position += 1uz;
				if (m_position < m_text.size() && (m_text[m_position] == '+' || m_text[m_position] == '-'))
				{
					m_position += 1uz;
				}
				const size_t digits = m_position;
				while (m_position < m_text.size() && m_text[m_position] >= '0' && m_text[m_position] <= '9')
				{
					m_position += 1uz;
				}
				if (m_position == digits)
				{
					return Fail("expected a digit in the exponent");
				}
			}

			// The grammar is already checked, so strtod reads exactly this slice.
			const std::string digits(m_text.substr(start, m_position - start));
			errno = 0;
			const double value = std::strtod(digits.c_str(), nullptr);
			if (errno == ERANGE && std::isinf(value))
			{
				m_position = start;
				return Fail("number out of range");
			}

			return JsonValue(value);
		}

		static void AppendUtf8(std::string& out, uint32_t code_point)
		{
			if (code_point < 0x80u)
			{
				out.push_back(static_cast<char>(code_point));
			}
			else if (code_point < 0x800u)
			{
				out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
				out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
			}
			else if (code_point < 0x10000u)
			{
				out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
				out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
				out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
				out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
			}
		}

		std::expected<uint32_t, std::string> ParseHex4()
		{
			if (m_position + 4uz > m_text.size())
			{
				return Fail("expected four hex digits after \\u");
			}

			uint32_t value = 0u;
			for (size_t index = 0uz; index < 4uz; index += 1uz)
			{
				const char ch = m_text[m_position + index];
				value <<= 4;
				if (ch >= '0' && ch <= '9')
				{
					value |= static_cast<uint32_t>(ch - '0');
				}
				else if (ch >= 'a' && ch <= 'f')
				{
					value |= static_cast<uint32_t>(ch - 'a' + 10);
				}
				else if (ch >= 'A' && ch <= 'F')
				{
					value |= static_cast<uint32_t>(ch - 'A' + 10);
				}
				else
				{
					return Fail("expected four hex digits after \\u");
				}
			}

			m_position += 4uz;
			return value;
		}

		std::expected<std::string, std::string> ParseString()
		{
			m_position += 1uz;
			std::string out;

			while (true)
			{
				if (m_position >= m_text.size())
				{
					return Fail("unterminated string");
				}

				const char ch = m_text[m_position];
				if (ch == '"')
				{
					m_position += 1uz;
					return out;
				}

				if (static_cast<unsigned char>(ch) < 0x20u)
				{
					return Fail("control character in a string; escape it");
				}

				if (ch != '\\')
				{
					out.push_back(ch);
					m_position += 1uz;
					continue;
				}

				m_position += 1uz;
				if (m_position >= m_text.size())
				{
					return Fail("unterminated escape");
				}

				const char escape = m_text[m_position];
				m_position += 1uz;
				switch (escape)
				{
				case '"':
					out.push_back('"');
					break;
				case '\\':
					out.push_back('\\');
					break;
				case '/':
					out.push_back('/');
					break;
				case 'b':
					out.push_back('\b');
					break;
				case 'f':
					out.push_back('\f');
					break;
				case 'n':
					out.push_back('\n');
					break;
				case 'r':
					out.push_back('\r');
					break;
				case 't':
					out.push_back('\t');
					break;
				case 'u':
				{
					std::expected<uint32_t, std::string> unit = ParseHex4();
					if (!unit.has_value())
					{
						return std::unexpected(unit.error());
					}

					uint32_t code_point = unit.value();
					if (code_point >= 0xD800u && code_point <= 0xDBFFu)
					{
						if (!Consume("\\u"))
						{
							return Fail("a high surrogate must be followed by a low surrogate");
						}
						std::expected<uint32_t, std::string> low = ParseHex4();
						if (!low.has_value())
						{
							return std::unexpected(low.error());
						}
						if (low.value() < 0xDC00u || low.value() > 0xDFFFu)
						{
							return Fail("a high surrogate must be followed by a low surrogate");
						}
						code_point = 0x10000u + ((code_point - 0xD800u) << 10) + (low.value() - 0xDC00u);
					}
					else if (code_point >= 0xDC00u && code_point <= 0xDFFFu)
					{
						return Fail("a low surrogate without a high surrogate");
					}

					AppendUtf8(out, code_point);
					break;
				}
				default:
					m_position -= 1uz;
					return Fail(std::format("unknown escape '\\{}'", escape));
				}
			}
		}
	};
}

namespace MidoriJson
{
	const JsonValue* JsonValue::Find(std::string_view key) const
	{
		if (!IsObject())
		{
			return nullptr;
		}

		for (const std::pair<std::string, JsonValue>& member : AsObject())
		{
			if (member.first == key)
			{
				return &member.second;
			}
		}

		return nullptr;
	}

	std::string_view JsonValue::KindName() const
	{
		if (IsNull())
		{
			return "null";
		}
		if (IsBool())
		{
			return "boolean";
		}
		if (IsNumber())
		{
			return "number";
		}
		if (IsString())
		{
			return "string";
		}
		if (IsArray())
		{
			return "array";
		}
		return "object";
	}

	std::expected<JsonValue, std::string> Parse(std::string_view text)
	{
		return JsonParser(text).ParseDocument();
	}
}
