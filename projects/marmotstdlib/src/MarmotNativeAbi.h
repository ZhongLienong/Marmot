#pragma once

#include <cstdint>

#if defined(_WIN32)
	#define MARMOT_FFI_CALL __cdecl
	#define MARMOT_FFI_EXPORT __declspec(dllexport)
#else
	#define MARMOT_FFI_CALL
	#define MARMOT_FFI_EXPORT __attribute__((visibility("default")))
#endif

struct MarmotFFIArray
{
	void* data;
	int32_t length;
};

using MarmotFFIFn = void (MARMOT_FFI_CALL*)(void**, void*);

inline constexpr uint8_t MARMOT_FFI_RETURN_ARRAY_VALUES = 2u;
inline constexpr uint8_t MARMOT_FFI_RETURN_ARRAY_STRINGS = 3u;

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr);
