#include "FFIAllocation/FFIAllocation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr)
{
	if (return_kind == MARMOT_FFI_RETURN_ARRAY_VALUES || return_kind == MARMOT_FFI_RETURN_ARRAY_STRINGS)
	{
		MarmotFFIArray* array = static_cast<MarmotFFIArray*>(ptr);
		if (return_kind == MARMOT_FFI_RETURN_ARRAY_STRINGS)
		{
			char** strings = static_cast<char**>(array->data);
			for (int32_t index = 0; index < array->length; index += 1)
			{
				std::free(strings[index]);
			}
		}
		std::free(array->data);
	}
	std::free(ptr);
}

namespace FFIAllocation
{
	char* AllocateString(std::string_view text)
	{
		char* buffer = static_cast<char*>(std::malloc(text.size() + 1u));
		if (buffer != nullptr)
		{
			std::memcpy(buffer, text.data(), text.size());
			buffer[text.size()] = '\0';
		}
		return buffer;
	}

	MarmotFFIArray* AllocateStringArray(const std::vector<std::string>& values)
	{
		if (values.empty())
		{
			return nullptr;
		}

		MarmotFFIArray* result = static_cast<MarmotFFIArray*>(std::malloc(sizeof(MarmotFFIArray)));
		char** data = static_cast<char**>(std::malloc(values.size() * sizeof(char*)));
		if (result == nullptr || data == nullptr)
		{
			std::free(result);
			std::free(data);
			return nullptr;
		}

		std::ranges::transform(values, data, AllocateString);
		*result = MarmotFFIArray{ data, static_cast<int>(values.size()) };
		return result;
	}
}
