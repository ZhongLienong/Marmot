#include "FFI/MarmotVMFFIExports.h"

#include "Error/RuntimeError.h"
#include "Value/Value.h"
#include "Library/ProgramArguments/ProgramArguments.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
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

	MarmotFFIArray* AllocateValueArray(int length) noexcept
	{
		if (length <= 0)
		{
			return nullptr;
		}

		MarmotValue* data = static_cast<MarmotValue*>(std::malloc(static_cast<size_t>(length) * sizeof(MarmotValue)));
		if (data == nullptr)
		{
			return nullptr;
		}

		MarmotFFIArray* result = static_cast<MarmotFFIArray*>(std::malloc(sizeof(MarmotFFIArray)));
		if (result == nullptr)
		{
			std::free(data);
			return nullptr;
		}

		result->data = data;
		result->length = length;
		return result;
	}

	MarmotFFIArray* AllocateValueArray(const MarmotArray& array) noexcept
	{
		MarmotFFIArray* result = AllocateValueArray(array.GetLength());
		if (result != nullptr)
		{
			std::memcpy(result->data, &array[0u], static_cast<size_t>(result->length) * sizeof(MarmotValue));
		}
		return result;
	}

	MarmotFFIArray* AllocateStringArray(const std::vector<MarmotText>& values) noexcept
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

		MarmotFFIArray* result = static_cast<MarmotFFIArray*>(std::malloc(sizeof(MarmotFFIArray)));
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
	void MARMOT_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept
	{
		MarmotArray concatenated = MarmotArray::Concatenate(RequireArray(args[0u]), RequireArray(args[1u]));
		WritePointerResult(ret, AllocateValueArray(concatenated));
	}

	void MARMOT_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept
	{
		WriteInt(ret, RequireArray(args[0u]).GetLength());
	}

	void MARMOT_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept
	{
		MarmotArray sliced = RequireArray(args[0u]).Slice(ReadInt(args, 1u), ReadInt(args, 2u));
		WritePointerResult(ret, AllocateValueArray(sliced));
	}

	void MARMOT_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept
	{
		MarmotArray reversed = RequireArray(args[0u]).Reverse();
		WritePointerResult(ret, AllocateValueArray(reversed));
	}

	void MARMOT_FFI_FUNC(ArrayConcatAll)(void** args, void* ret) noexcept
	{
		const MarmotArray& chunks = RequireArray(args[0u]);
		MarmotInteger length = 0;
		for (int index = 0; index < chunks.GetLength(); index += 1)
		{
			length += RequireArray(chunks[index].GetPointer()).GetLength();
			if (length > std::numeric_limits<int>::max())
			{
				FatalOutOfMemory("concatenating arrays", static_cast<size_t>(length) * sizeof(MarmotValue));
			}
		}
		if (length == 0)
		{
			WritePointerResult(ret, nullptr);
			return;
		}

		MarmotFFIArray* result = AllocateValueArray(static_cast<int>(length));
		if (result == nullptr)
		{
			FatalOutOfMemory("concatenating arrays", static_cast<size_t>(length) * sizeof(MarmotValue));
		}

		MarmotValue* destination = static_cast<MarmotValue*>(result->data);
		for (int index = 0; index < chunks.GetLength(); index += 1)
		{
			const MarmotArray& chunk = RequireArray(chunks[index].GetPointer());
			const int count = chunk.GetLength();
			if (count > 0)
			{
				std::memcpy(destination, &chunk[0u], static_cast<size_t>(count) * sizeof(MarmotValue));
				destination += count;
			}
		}
		WritePointerResult(ret, result);
	}

	void MARMOT_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept
	{
		MarmotText concatenated = MarmotText::Concatenate(RequireText(args[0u]), RequireText(args[1u]));
		WritePointerResult(ret, AllocateCString(concatenated));
	}

	void MARMOT_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept
	{
		MarmotText substring = RequireText(args[0u]).Substring(ReadInt(args, 1u), ReadInt(args, 2u));
		WritePointerResult(ret, AllocateCString(substring));
	}

	void MARMOT_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept
	{
		std::vector<MarmotText> parts = RequireText(args[0u]).Split(RequireText(args[1u]));
		WritePointerResult(ret, AllocateStringArray(parts));
	}

	void MARMOT_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept
	{
		MarmotText reversed = RequireText(args[0u]).Reverse();
		WritePointerResult(ret, AllocateCString(reversed));
	}

	// The UTF-8 for one Unicode scalar value. The caller has checked it is one,
	// and that it is not U+0000, which text cannot hold.
	void MARMOT_FFI_FUNC(TextFromCodePoint)(void** args, void* ret) noexcept
	{
		const std::array<MarmotInteger, 1u> code_point{ ReadInt(args, 0u) };
		WritePointerResult(ret, AllocateCString(MarmotText::FromCodePoints(code_point)));
	}

	void MARMOT_FFI_FUNC(TextCodePoints)(void** args, void* ret) noexcept
	{
		const std::vector<MarmotInteger> code_points = RequireText(args[0u]).CodePoints();
		MarmotArray values(static_cast<int>(code_points.size()));
		for (int index = 0; index < values.GetLength(); index += 1)
		{
			values[index] = MarmotValue(code_points[static_cast<size_t>(index)]);
		}
		WritePointerResult(ret, AllocateValueArray(values));
	}

	void MARMOT_FFI_FUNC(TextFromCodePoints)(void** args, void* ret) noexcept
	{
		const MarmotArray& values = RequireArray(args[0u]);
		const std::vector<MarmotInteger> code_points = std::views::iota(0, values.GetLength())
			| std::views::transform([&values](int index) { return values[index].GetInteger(); })
			| std::ranges::to<std::vector>();
		WritePointerResult(ret, AllocateCString(MarmotText::FromCodePoints(code_points)));
	}

	// The code point at a code-point index, or -1 past the end.
	void MARMOT_FFI_FUNC(TextCodePointAt)(void** args, void* ret) noexcept
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

	void MARMOT_FFI_FUNC(TextIsInteger)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).ParseInteger().has_value());
	}

	void MARMOT_FFI_FUNC(TextIsFloat)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).ParseFloat().has_value());
	}

	// UTF-8 byte order is code point order.
	void MARMOT_FFI_FUNC(TextCompare)(void** args, void* ret) noexcept
	{
		const int order = RequireText(args[0u]).View().compare(RequireText(args[1u]).View());
		WriteInt(ret, (order > 0) - (order < 0));
	}

	void MARMOT_FFI_FUNC(TextContains)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).Contains(RequireText(args[1u])));
	}

	void MARMOT_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept
	{
		MarmotText replaced = RequireText(args[0u]).Replace(RequireText(args[1u]), RequireText(args[2u]));
		WritePointerResult(ret, AllocateCString(replaced));
	}

	void MARMOT_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept
	{
		MarmotText trimmed = RequireText(args[0u]).Trim();
		WritePointerResult(ret, AllocateCString(trimmed));
	}

	void MARMOT_FFI_FUNC(GetArguments)(void**, void* ret) noexcept
	{
		const std::vector<std::string>& arguments = ProgramArguments::Get();
		std::vector<MarmotText> texts;
		texts.reserve(arguments.size());
		std::ranges::transform(arguments, std::back_inserter(texts), [](const std::string& argument) { return MarmotText(argument.c_str()); });
		WritePointerResult(ret, AllocateStringArray(texts));
	}

	void MARMOT_FFI_FUNC(TextLength)(void** args, void* ret) noexcept
	{
		WriteInt(ret, RequireText(args[0u]).GetLength());
	}

	void MARMOT_FFI_FUNC(HashText)(void** args, void* ret) noexcept
	{
		WriteInt(ret, static_cast<MarmotInteger>(std::hash<std::string_view>{}(RequireText(args[0u]).View())));
	}

	void MARMOT_FFI_FUNC(HashFloat)(void** args, void* ret) noexcept
	{
		double value = 0.0;
		std::memcpy(&value, &args[0u], sizeof(value));
		WriteInt(ret, static_cast<MarmotInteger>(std::hash<double>{}(value)));
	}

	void MARMOT_FFI_FUNC(ReportPanic)(void** args, void*) noexcept
	{
		std::cerr << RequireText(args[0u]).View();
	}
}
