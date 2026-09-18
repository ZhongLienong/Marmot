#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace MidoriJson
{
	[[nodiscard]] inline std::string EscapeString(std::string_view value)
	{
		std::string escaped;
		escaped.reserve(value.size());

		for (const unsigned char ch : value)
		{
			switch (ch)
			{
			case '\"':
				escaped += "\\\"";
				break;
			case '\\':
				escaped += "\\\\";
				break;
			case '\b':
				escaped += "\\b";
				break;
			case '\f':
				escaped += "\\f";
				break;
			case '\n':
				escaped += "\\n";
				break;
			case '\r':
				escaped += "\\r";
				break;
			case '\t':
				escaped += "\\t";
				break;
			default:
				if (ch < 0x20u)
				{
					escaped += "\\u00";
					constexpr char hex_digits[] = "0123456789ABCDEF";
					escaped.push_back(hex_digits[(ch >> 4) & 0x0Fu]);
					escaped.push_back(hex_digits[ch & 0x0Fu]);
				}
				else
				{
					escaped.push_back(static_cast<char>(ch));
				}
				break;
			}
		}

		return escaped;
	}

	inline void AppendFieldPrefix(std::string& out, std::string_view key, bool& first_field)
	{
		if (!first_field)
		{
			out.push_back(',');
		}
		first_field = false;

		out.push_back('\"');
		out += key;
		out += "\":";
	}

	inline void AppendRawField(std::string& out, std::string_view key, std::string_view raw_json, bool& first_field)
	{
		AppendFieldPrefix(out, key, first_field);
		out += raw_json;
	}

	inline void AppendBoolField(std::string& out, std::string_view key, bool value, bool& first_field)
	{
		AppendFieldPrefix(out, key, first_field);
		out += value ? "true" : "false";
	}

	inline void AppendStringField(std::string& out, std::string_view key, std::optional<std::string_view> value, bool& first_field)
	{
		AppendFieldPrefix(out, key, first_field);
		if (!value.has_value())
		{
			out += "null";
			return;
		}

		out.push_back('\"');
		out += EscapeString(*value);
		out.push_back('\"');
	}

	inline void AppendStringField(std::string& out, std::string_view key, std::string_view value, bool& first_field)
	{
		AppendStringField(out, key, std::optional<std::string_view>(value), first_field);
	}

	inline void AppendStringField(std::string& out, std::string_view key, const std::string& value, bool& first_field)
	{
		AppendStringField(out, key, std::string_view(value), first_field);
	}

	inline void AppendStringField(std::string& out, std::string_view key, const char* value, bool& first_field)
	{
		AppendStringField(out, key, std::string_view(value), first_field);
	}

	template <typename NumberType>
	inline void AppendNumberField(std::string& out, std::string_view key, std::optional<NumberType> value, bool& first_field)
	{
		AppendFieldPrefix(out, key, first_field);
		if (!value.has_value())
		{
			out += "null";
			return;
		}

		out += std::to_string(*value);
	}

	template <typename NumberType>
	inline void AppendNumberField(std::string& out, std::string_view key, NumberType value, bool& first_field)
	{
		AppendNumberField(out, key, std::optional<NumberType>(value), first_field);
	}
}
