#include "Library/MarmotStdLibExports.h"

#include "Error/RuntimeError.h"
#include "Value/Value.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	struct FFIArray
	{
		void* data;
		int length;
	};

	[[noreturn]] void AbortRuntime(std::string_view message) noexcept
	{
		const RuntimeError runtime_error(RuntimeErrorCode::InternalFFITypeError, message);
		const std::string rendered(runtime_error.Rendered());
		std::fputs(rendered.c_str(), stderr);
		std::fputc('\n', stderr);
		std::fflush(stderr);
		std::exit(EXIT_FAILURE);
	}

	void WriteBool(void* ret, bool value) noexcept
	{
		*static_cast<MarmotValue*>(ret) = MarmotValue(value);
	}

	void WriteInt(void* ret, MarmotInteger value) noexcept
	{
		*static_cast<MarmotValue*>(ret) = MarmotValue(value);
	}

	void WritePointerResult(void* ret, const void* pointer) noexcept
	{
		*static_cast<MarmotValue*>(ret) = static_cast<MarmotInteger>(reinterpret_cast<int64_t>(pointer));
	}

	const MarmotValue& RequireValueArg(void** args, size_t index) noexcept
	{
		MarmotValue* value = static_cast<MarmotValue*>(args[index]);
		if (value == nullptr)
		{
			AbortRuntime("Built-in FFI expected value argument.");
		}
		return *value;
	}

	MarmotInteger ReadInt(void** args, size_t index) noexcept
	{
		return RequireValueArg(args, index).GetInteger();
	}

	MarmotTraceable* RequireTraceable(void* handle, const char* expected_type) noexcept
	{
		MarmotTraceable* traceable = static_cast<MarmotTraceable*>(handle);
		if (traceable == nullptr)
		{
			AbortRuntime(std::string("Built-in FFI expected ") + expected_type + ".");
		}
		return traceable;
	}

	MarmotArray& RequireArray(void* handle) noexcept
	{
		MarmotTraceable* traceable = RequireTraceable(handle, "Array");
		if (!traceable->IsTraceable<MarmotArray>())
		{
			AbortRuntime("Built-in FFI expected Array.");
		}
		return traceable->GetTraceable<MarmotArray>();
	}

	MarmotText& RequireText(void* handle) noexcept
	{
		MarmotTraceable* traceable = RequireTraceable(handle, "Text");
		if (!traceable->IsTraceable<MarmotText>())
		{
			AbortRuntime("Built-in FFI expected Text.");
		}
		return traceable->GetTraceable<MarmotText>();
	}

	char* AllocateCString(const MarmotText& text) noexcept
	{
		const int byte_len = text.GetByteLength();
		char* buffer = static_cast<char*>(std::malloc(static_cast<size_t>(byte_len) + 1u));
		if (buffer == nullptr)
		{
			return nullptr;
		}

		std::memcpy(buffer, text.View().data(), static_cast<size_t>(byte_len));
		buffer[byte_len] = '\0';
		return buffer;
	}

	FFIArray* AllocateValueArray(const MarmotArray& array) noexcept
	{
		const int length = array.GetLength();
		if (length <= 0)
		{
			return nullptr;
		}

		MarmotValue* data = static_cast<MarmotValue*>(std::malloc(static_cast<size_t>(length) * sizeof(MarmotValue)));
		if (data == nullptr)
		{
			return nullptr;
		}

		std::memcpy(data, &array[0u], static_cast<size_t>(length) * sizeof(MarmotValue));

		FFIArray* result = static_cast<FFIArray*>(std::malloc(sizeof(FFIArray)));
		if (result == nullptr)
		{
			std::free(data);
			return nullptr;
		}

		result->data = data;
		result->length = length;
		return result;
	}

	FFIArray* AllocateStringArray(const std::vector<MarmotText>& values) noexcept
	{
		if (values.empty())
		{
			return nullptr;
		}

		char** data = static_cast<char**>(std::malloc(values.size() * sizeof(char*)));
		if (data == nullptr)
		{
			return nullptr;
		}

		size_t initialized = 0u;
		for (; initialized < values.size(); initialized += 1u)
		{
			data[initialized] = AllocateCString(values[initialized]);
			if (data[initialized] == nullptr)
			{
				for (size_t idx = 0u; idx < initialized; idx += 1u)
				{
					std::free(data[idx]);
				}
				std::free(data);
				return nullptr;
			}
		}

		FFIArray* result = static_cast<FFIArray*>(std::malloc(sizeof(FFIArray)));
		if (result == nullptr)
		{
			for (size_t idx = 0u; idx < values.size(); idx += 1u)
			{
				std::free(data[idx]);
			}
			std::free(data);
			return nullptr;
		}

		result->data = data;
		result->length = static_cast<int>(values.size());
		return result;
	}
}

extern "C"
{
	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept
	{
		MarmotArray concatenated = MarmotArray::Concatenate(RequireArray(args[0u]), RequireArray(args[1u]));
		WritePointerResult(ret, AllocateValueArray(concatenated));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept
	{
		WriteInt(ret, RequireArray(args[0u]).GetLength());
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept
	{
		MarmotArray sliced = RequireArray(args[0u]).Slice(ReadInt(args, 1u), ReadInt(args, 2u));
		WritePointerResult(ret, AllocateValueArray(sliced));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept
	{
		MarmotArray reversed = RequireArray(args[0u]).Reverse();
		WritePointerResult(ret, AllocateValueArray(reversed));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept
	{
		MarmotText concatenated = MarmotText::Concatenate(RequireText(args[0u]), RequireText(args[1u]));
		WritePointerResult(ret, AllocateCString(concatenated));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept
	{
		MarmotText substring = RequireText(args[0u]).Substring(ReadInt(args, 1u), ReadInt(args, 2u));
		WritePointerResult(ret, AllocateCString(substring));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept
	{
		std::vector<MarmotText> parts = RequireText(args[0u]).Split(RequireText(args[1u]));
		WritePointerResult(ret, AllocateStringArray(parts));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept
	{
		MarmotText reversed = RequireText(args[0u]).Reverse();
		WritePointerResult(ret, AllocateCString(reversed));
	}

	// The UTF-8 for one Unicode scalar value. The caller has checked it is one,
	// and that it is not U+0000, which text cannot hold.
	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextFromCodePoint)(void** args, void* ret) noexcept
	{
		const std::array<MarmotInteger, 1u> code_point{ ReadInt(args, 0u) };
		WritePointerResult(ret, AllocateCString(MarmotText::FromCodePoints(code_point)));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextCodePoints)(void** args, void* ret) noexcept
	{
		const std::vector<MarmotInteger> code_points = RequireText(args[0u]).CodePoints();
		MarmotArray values(static_cast<int>(code_points.size()));
		for (int index = 0; index < values.GetLength(); index += 1)
		{
			values[index] = MarmotValue(code_points[static_cast<size_t>(index)]);
		}
		WritePointerResult(ret, AllocateValueArray(values));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextFromCodePoints)(void** args, void* ret) noexcept
	{
		const MarmotArray& values = RequireArray(args[0u]);
		const std::vector<MarmotInteger> code_points = std::views::iota(0, values.GetLength())
			| std::views::transform([&values](int index) { return values[index].GetInteger(); })
			| std::ranges::to<std::vector>();
		WritePointerResult(ret, AllocateCString(MarmotText::FromCodePoints(code_points)));
	}

	// The code point at a code-point index, or -1 past the end.
	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextCodePointAt)(void** args, void* ret) noexcept
	{
		const MarmotText& text = RequireText(args[0u]);
		const MarmotInteger index = ReadInt(args, 1u);
		if ((index < 0) || (index >= text.GetLength()))
		{
			WriteInt(ret, -1);
			return;
		}

		const MarmotText character = text.Substring(index, index + 1);
		const unsigned char* bytes = reinterpret_cast<const unsigned char*>(character.View().data());
		const int length = character.GetByteLength();
		std::uint32_t code_point = (length == 1) ? bytes[0]
			: (length == 2) ? (bytes[0] & 0x1Fu)
			: (length == 3) ? (bytes[0] & 0x0Fu)
			: (bytes[0] & 0x07u);
		for (int continuation = 1; continuation < length; continuation += 1)
		{
			code_point = (code_point << 6u) | (bytes[continuation] & 0x3Fu);
		}
		WriteInt(ret, static_cast<MarmotInteger>(code_point));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextIsInteger)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).ParseInteger().has_value());
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextIsFloat)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).ParseFloat().has_value());
	}

	// UTF-8 byte order is code point order.
	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextCompare)(void** args, void* ret) noexcept
	{
		const int order = RequireText(args[0u]).View().compare(RequireText(args[1u]).View());
		WriteInt(ret, (order > 0) - (order < 0));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextContains)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).Contains(RequireText(args[1u])));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept
	{
		MarmotText replaced = RequireText(args[0u]).Replace(RequireText(args[1u]), RequireText(args[2u]));
		WritePointerResult(ret, AllocateCString(replaced));
	}

	MARMOT_STDLIB_API void MARMOT_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept
	{
		MarmotText trimmed = RequireText(args[0u]).Trim();
		WritePointerResult(ret, AllocateCString(trimmed));
	}
}
