#include "Library/MidoriStdLibExports.h"

#include "Common/Error/Error.h"
#include "Common/Value/Value.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
		const RuntimeError runtime_error = MidoriError::GenerateRuntimeError(RuntimeErrorCode::InternalFFITypeError, message);
		const std::string rendered(runtime_error.Rendered());
		std::fputs(rendered.c_str(), stderr);
		std::fputc('\n', stderr);
		std::fflush(stderr);
		std::exit(EXIT_FAILURE);
	}

	void WriteUnit(void* ret) noexcept
	{
		*static_cast<MidoriValue*>(ret) = MidoriValue();
	}

	void WriteBool(void* ret, bool value) noexcept
	{
		*static_cast<MidoriValue*>(ret) = MidoriValue(value);
	}

	void WriteInt(void* ret, MidoriInteger value) noexcept
	{
		*static_cast<MidoriValue*>(ret) = MidoriValue(value);
	}

	void WritePointerResult(void* ret, const void* pointer) noexcept
	{
		*static_cast<MidoriValue*>(ret) = static_cast<MidoriInteger>(reinterpret_cast<int64_t>(pointer));
	}

	const MidoriValue& RequireValueArg(void** args, size_t index) noexcept
	{
		MidoriValue* value = static_cast<MidoriValue*>(args[index]);
		if (value == nullptr)
		{
			AbortRuntime("Built-in FFI expected value argument.");
		}
		return *value;
	}

	MidoriInteger ReadInt(void** args, size_t index) noexcept
	{
		return RequireValueArg(args, index).GetInteger();
	}

	MidoriValue ReadValue(void** args, size_t index) noexcept
	{
		return RequireValueArg(args, index);
	}

	MidoriTraceable* RequireTraceable(void* handle, const char* expected_type) noexcept
	{
		MidoriTraceable* traceable = static_cast<MidoriTraceable*>(handle);
		if (traceable == nullptr)
		{
			AbortRuntime(std::string("Built-in FFI expected ") + expected_type + ".");
		}
		return traceable;
	}

	MidoriArray& RequireArray(void* handle) noexcept
	{
		MidoriTraceable* traceable = RequireTraceable(handle, "Array");
		if (!traceable->IsTraceable<MidoriArray>())
		{
			AbortRuntime("Built-in FFI expected Array.");
		}
		return traceable->GetTraceable<MidoriArray>();
	}

	MidoriText& RequireText(void* handle) noexcept
	{
		MidoriTraceable* traceable = RequireTraceable(handle, "Text");
		if (!traceable->IsTraceable<MidoriText>())
		{
			AbortRuntime("Built-in FFI expected Text.");
		}
		return traceable->GetTraceable<MidoriText>();
	}

	char* AllocateCString(const MidoriText& text) noexcept
	{
		const int byte_len = text.GetByteLength();
		char* buffer = static_cast<char*>(std::malloc(static_cast<size_t>(byte_len) + 1u));
		if (buffer == nullptr)
		{
			return nullptr;
		}

		std::memcpy(buffer, text.GetCString(), static_cast<size_t>(byte_len));
		buffer[byte_len] = '\0';
		return buffer;
	}

	FFIArray* AllocateValueArray(const MidoriArray& array) noexcept
	{
		const int length = array.GetLength();
		if (length <= 0)
		{
			return nullptr;
		}

		MidoriValue* data = static_cast<MidoriValue*>(std::malloc(static_cast<size_t>(length) * sizeof(MidoriValue)));
		if (data == nullptr)
		{
			return nullptr;
		}

		std::memcpy(data, &array[0u], static_cast<size_t>(length) * sizeof(MidoriValue));

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

	FFIArray* AllocateStringArray(const std::vector<MidoriText>& values) noexcept
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
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept
	{
		MidoriArray concatenated = MidoriArray::Concatenate(RequireArray(args[0u]), RequireArray(args[1u]));
		WritePointerResult(ret, AllocateValueArray(concatenated));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept
	{
		WriteInt(ret, RequireArray(args[0u]).GetLength());
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept
	{
		MidoriArray sliced = RequireArray(args[0u]).Slice(static_cast<int>(ReadInt(args, 1u)), static_cast<int>(ReadInt(args, 2u)));
		WritePointerResult(ret, AllocateValueArray(sliced));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept
	{
		MidoriArray reversed = RequireArray(args[0u]).Reverse();
		WritePointerResult(ret, AllocateValueArray(reversed));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayContains)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireArray(args[0u]).Contains(ReadValue(args, 1u)));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept
	{
		MidoriText concatenated = MidoriText::Concatenate(RequireText(args[0u]), RequireText(args[1u]));
		WritePointerResult(ret, AllocateCString(concatenated));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept
	{
		MidoriText substring = RequireText(args[0u]).Substring(static_cast<int>(ReadInt(args, 1u)), static_cast<int>(ReadInt(args, 2u)));
		WritePointerResult(ret, AllocateCString(substring));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept
	{
		std::vector<MidoriText> parts = RequireText(args[0u]).Split(RequireText(args[1u]));
		WritePointerResult(ret, AllocateStringArray(parts));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept
	{
		MidoriText reversed = RequireText(args[0u]).Reverse();
		WritePointerResult(ret, AllocateCString(reversed));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextContains)(void** args, void* ret) noexcept
	{
		WriteBool(ret, RequireText(args[0u]).Contains(RequireText(args[1u])));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept
	{
		MidoriText replaced = RequireText(args[0u]).Replace(RequireText(args[1u]), RequireText(args[2u]));
		WritePointerResult(ret, AllocateCString(replaced));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept
	{
		MidoriText trimmed = RequireText(args[0u]).Trim();
		WritePointerResult(ret, AllocateCString(trimmed));
	}
}
