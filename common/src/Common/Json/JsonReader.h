#pragma once

#include <concepts>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace MidoriJson
{
	// A parsed JSON document. Objects keep their members in source order and
	// reject duplicate keys, so a reader can report exactly what it was given.
	class JsonValue
	{
	public:
		using Array = std::vector<JsonValue>;
		using Object = std::vector<std::pair<std::string, JsonValue>>;

	private:
		std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_value;

	public:
		JsonValue() : m_value(nullptr) {}

		template<typename T>
			requires (!std::same_as<std::remove_cvref_t<T>, JsonValue>)
		explicit JsonValue(T&& value) : m_value(std::forward<T>(value)) {}

		[[nodiscard]] bool IsNull() const { return std::holds_alternative<std::nullptr_t>(m_value); }
		[[nodiscard]] bool IsBool() const { return std::holds_alternative<bool>(m_value); }
		[[nodiscard]] bool IsNumber() const { return std::holds_alternative<double>(m_value); }
		[[nodiscard]] bool IsString() const { return std::holds_alternative<std::string>(m_value); }
		[[nodiscard]] bool IsArray() const { return std::holds_alternative<Array>(m_value); }
		[[nodiscard]] bool IsObject() const { return std::holds_alternative<Object>(m_value); }

		[[nodiscard]] bool AsBool() const { return std::get<bool>(m_value); }
		[[nodiscard]] double AsNumber() const { return std::get<double>(m_value); }
		[[nodiscard]] const std::string& AsString() const { return std::get<std::string>(m_value); }
		[[nodiscard]] const Array& AsArray() const { return std::get<Array>(m_value); }
		[[nodiscard]] const Object& AsObject() const { return std::get<Object>(m_value); }

		// The member named `key` of an object, or nullptr.
		[[nodiscard]] const JsonValue* Find(std::string_view key) const;

		// "null", "boolean", "number", "string", "array" or "object".
		[[nodiscard]] std::string_view KindName() const;
	};

	// Parses one JSON document (RFC 8259). Errors name the line and column.
	[[nodiscard]] std::expected<JsonValue, std::string> Parse(std::string_view text);
}
