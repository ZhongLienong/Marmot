#include "Compiler/Json/JsonReader.h"
#include "Support/Expected.h"

#include <algorithm>
#include <format>
#include <memory>
#include <unordered_set>

#include <yyjson.h>

namespace
{
	using CompilerJson::JsonValue;
	using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
	constexpr int MAX_DEPTH = 128;

	std::string ErrorAt(std::string_view text, size_t position, std::string_view message)
	{
		const std::string_view preceding = text.substr(0, position);
		const size_t newline = preceding.rfind('\n');
		const size_t column = newline == std::string_view::npos ? preceding.size() + 1u : preceding.size() - newline;
		return std::format("line {}, column {}: {}", std::ranges::count(preceding, '\n') + 1, column, message);
	}

	// yyjson retains keys in source order, but only syntax errors carry byte offsets.
	// Locate a rejected key by its ordinal, using yyjson to consume each string token.
	size_t KeyPosition(std::string_view text, size_t ordinal)
	{
		for (size_t offset = 0; ; )
		{
			const size_t start = text.find('"', offset);
			const Document token(yyjson_read(text.data() + start, text.size() - start, YYJSON_READ_STOP_WHEN_DONE), yyjson_doc_free);
			offset = start + yyjson_doc_get_read_size(token.get());
			const size_t next = text.find_first_not_of(" \t\r\n", offset);
			if (next != std::string_view::npos && text[next] == ':' && --ordinal == 0)
			{
				return start;
			}
		}
	}

	MarmotCompiler::Expected<JsonValue, std::string> Convert(yyjson_val* value, std::string_view text, int depth, size_t& keys)
	{
		if (depth > MAX_DEPTH)
		{
			return MarmotCompiler::Failure(std::string("JSON nested too deeply"));
		}

		switch (yyjson_get_type(value))
		{
		case YYJSON_TYPE_NULL:
			return JsonValue();
		case YYJSON_TYPE_BOOL:
			return JsonValue(yyjson_get_bool(value));
		case YYJSON_TYPE_NUM:
			return JsonValue(yyjson_get_num(value));
		case YYJSON_TYPE_STR:
			return JsonValue(std::string(yyjson_get_str(value), yyjson_get_len(value)));
		case YYJSON_TYPE_ARR:
		{
			JsonValue::Array array;
			yyjson_arr_iter iterator = yyjson_arr_iter_with(value);
			while (yyjson_val* item = yyjson_arr_iter_next(&iterator))
			{
				MarmotCompiler::Expected<JsonValue, std::string> converted = Convert(item, text, depth + 1, keys);
				if (!converted)
				{
					return MarmotCompiler::Failure(std::move(converted.Error()));
				}
				array.push_back(std::move(*converted));
			}
			return JsonValue(std::move(array));
		}
		case YYJSON_TYPE_OBJ:
		{
			JsonValue::Object object;
			std::unordered_set<std::string_view> seen;
			yyjson_obj_iter iterator = yyjson_obj_iter_with(value);
			while (yyjson_val* key = yyjson_obj_iter_next(&iterator))
			{
				const std::string_view name(yyjson_get_str(key), yyjson_get_len(key));
				++keys;
				if (!seen.insert(name).second)
				{
					return MarmotCompiler::Failure(ErrorAt(text, KeyPosition(text, keys), std::format("duplicate member \"{}\"", name)));
				}
				MarmotCompiler::Expected<JsonValue, std::string> converted = Convert(yyjson_obj_iter_get_val(key), text, depth + 1, keys);
				if (!converted)
				{
					return MarmotCompiler::Failure(std::move(converted.Error()));
				}
				object.emplace_back(name, std::move(*converted));
			}
			return JsonValue(std::move(object));
		}
		}
		std::unreachable();
	}
}

namespace CompilerJson
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

	MarmotCompiler::Expected<JsonValue, std::string> Parse(std::string_view text)
	{
		yyjson_read_err error{};
		const Document document(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_NOFLAG, nullptr, &error), yyjson_doc_free);
		if (!document)
		{
			return MarmotCompiler::Failure(ErrorAt(text, error.pos, error.msg));
		}

		size_t keys = 0;
		return Convert(yyjson_doc_get_root(document.get()), text, 0, keys);
	}
}
