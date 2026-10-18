#include "Library/FFIAllocation/FFIAllocation.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

	FFIArray* AllocateStringArray(const std::vector<std::string>& values)
	{
		if (values.empty())
		{
			return nullptr;
		}

		FFIArray* result = static_cast<FFIArray*>(std::malloc(sizeof(FFIArray)));
		char** data = static_cast<char**>(std::malloc(values.size() * sizeof(char*)));
		if (result == nullptr || data == nullptr)
		{
			std::free(result);
			std::free(data);
			return nullptr;
		}

		std::ranges::transform(values, data, AllocateString);
		*result = FFIArray{ data, static_cast<int>(values.size()) };
		return result;
	}
}
