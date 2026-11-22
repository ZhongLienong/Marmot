#pragma once

#include <concepts>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <yyjson.h>

namespace MarmotJson
{
	class Document
	{
	public:
		explicit Document(bool array = false)
			: m_document(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free)
		{
			if (!m_document)
			{
				throw std::bad_alloc();
			}
			m_root = array ? yyjson_mut_arr(m_document.get()) : yyjson_mut_obj(m_document.get());
			if (m_root == nullptr)
			{
				throw std::bad_alloc();
			}
			yyjson_mut_doc_set_root(m_document.get(), m_root);
		}

		[[nodiscard]] static Document Array()
		{
			return Document(true);
		}

		void AddStringField(std::string_view name, std::optional<std::string_view> value)
		{
			AddField(name, value ? String(*value) : yyjson_mut_null(m_document.get()));
		}

		void AddBoolField(std::string_view name, bool value)
		{
			AddField(name, yyjson_mut_bool(m_document.get(), value));
		}

		template<std::integral Number>
		void AddNumberField(std::string_view name, std::optional<Number> value)
		{
			if (value)
			{
				AddNumberField(name, *value);
			}
			else
			{
				AddField(name, yyjson_mut_null(m_document.get()));
			}
		}

		template<std::integral Number>
		void AddNumberField(std::string_view name, Number value)
		{
			if constexpr (std::is_signed_v<Number>)
			{
				AddField(name, yyjson_mut_sint(m_document.get(), value));
			}
			else
			{
				AddField(name, yyjson_mut_uint(m_document.get(), value));
			}
		}

		void AddRawField(std::string_view name, std::string_view json)
		{
			AddField(name, Raw(json));
		}

		void AddField(std::string_view name, const Document& value)
		{
			AddField(name, yyjson_mut_val_mut_copy(m_document.get(), value.m_root));
		}

		void Append(const Document& value)
		{
			Append(yyjson_mut_val_mut_copy(m_document.get(), value.m_root));
		}

		void AppendString(std::string_view value)
		{
			Append(String(value));
		}

		void AppendRaw(std::string_view json)
		{
			Append(Raw(json));
		}

		[[nodiscard]] std::string Write() const
		{
			size_t size = 0;
			const std::unique_ptr<char, decltype(&std::free)> text(
				yyjson_mut_write(m_document.get(), YYJSON_WRITE_ALLOW_INVALID_UNICODE | YYJSON_WRITE_ESCAPE_UNICODE, &size), std::free);
			if (!text)
			{
				throw std::bad_alloc();
			}
			return std::string(text.get(), size);
		}

	private:
		std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)> m_document;
		yyjson_mut_val* m_root;

		[[nodiscard]] yyjson_mut_val* String(std::string_view value)
		{
			return yyjson_mut_strncpy(m_document.get(), value.empty() ? "" : value.data(), value.size());
		}

		[[nodiscard]] yyjson_mut_val* Raw(std::string_view json)
		{
			return yyjson_mut_rawncpy(m_document.get(), json.data(), json.size());
		}

		void AddField(std::string_view name, yyjson_mut_val* value)
		{
			if (!yyjson_mut_obj_add(m_root, String(name), value))
			{
				throw std::bad_alloc();
			}
		}

		void Append(yyjson_mut_val* value)
		{
			if (!yyjson_mut_arr_append(m_root, value))
			{
				throw std::bad_alloc();
			}
		}
	};
}
