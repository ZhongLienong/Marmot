#pragma once

#include "FFIAllocation/FFIAllocation.h"
#include "MarmotStdLibExports.h"

#if defined(_WIN32) || defined(_WIN64)
#define MARMOT_VM_FFI_API __declspec(dllexport)
#else
#define MARMOT_VM_FFI_API
#endif

extern "C"
{
	// VM adapters and operations on managed values.
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(Exit)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(Sleep)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(RunProcess)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(GetArguments)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(ArrayConcatAll)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextContains)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextFromCodePoint)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextCodePointAt)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextIsInteger)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextIsFloat)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextCompare)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextCodePoints)(void** args, void* ret) noexcept;
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(TextFromCodePoints)(void** args, void* ret) noexcept;
}
