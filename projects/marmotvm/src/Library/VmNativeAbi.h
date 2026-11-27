#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
	#define MARMOT_VM_NATIVE_CALL __cdecl
#else
	#define MARMOT_VM_NATIVE_CALL
#endif

namespace VmNativeAbi
{
	struct Array
	{
		void* m_data;
		int32_t m_length;
	};

	static_assert(offsetof(Array, m_length) == 8u && sizeof(Array) == 16u);

	using Function = void (MARMOT_VM_NATIVE_CALL*)(void**, void*);
	using FreeResult = void (MARMOT_VM_NATIVE_CALL*)(uint8_t, void*);

	enum ReturnKind : uint8_t
	{
		RETURN_SCALAR = 0,
		RETURN_STRING = 1,
		RETURN_ARRAY_VALUES = 2,
		RETURN_ARRAY_STRINGS = 3
	};
}
