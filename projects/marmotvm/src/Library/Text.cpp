#include "Library/MidoriStdLibExports.h"

#include <cstdint>
#include <cstring>

namespace
{
	int64_t CountCodePoints(const char* text) noexcept
	{
		if (text == nullptr)
		{
			return 0;
		}

		int64_t count = 0;
		const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text);
		while (*ptr != 0u)
		{
			// Count UTF-8 leading bytes.
			if ((*ptr & 0xC0u) != 0x80u)
			{
				count += 1;
			}
			ptr += 1;
		}

		return count;
	}
}

extern "C"
{
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextLength)(void** args, void* ret) noexcept
	{
		const char* text = reinterpret_cast<const char*>(args[0u]);
		int64_t length = CountCodePoints(text);
		std::memcpy(ret, &length, sizeof(int64_t));
	}
}
