#include "Library/MidoriStdLibExports.h"

#include <string>

extern "C"
{
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(HashText)(void** args, void* ret) noexcept
	{
		const char* str = reinterpret_cast<const char*>(args[0u]);
		int64_t hash = 0;
		if (str != nullptr)
		{
			hash = static_cast<int64_t>(std::hash<std::string>{}(str));
		}
		std::memcpy(ret, &hash, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(HashFloat)(void** args, void* ret) noexcept
	{
		double val = 0.0;
		std::memcpy(&val, &args[0u], sizeof(double));
		const int64_t hash = static_cast<int64_t>(std::hash<double>{}(val));
		std::memcpy(ret, &hash, sizeof(int64_t));
	}
}

