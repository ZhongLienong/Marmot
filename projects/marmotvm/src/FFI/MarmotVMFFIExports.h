#pragma once

#include "marmot/MarmotFFI.h"

#define MARMOT_FFI_FUNC(name) MARMOT_FFI_##name

extern "C"
{
	// VM adapters and operations on managed values.
	void MARMOT_FFI_FUNC(Exit)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(Sleep)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(GetArguments)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ArrayConcatAll)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextContains)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextFromCodePoint)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextCodePointAt)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextIsInteger)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextIsFloat)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextCompare)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextCodePoints)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextFromCodePoints)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(TextLength)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(HashText)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(HashFloat)(void** args, void* ret) noexcept;
	void MARMOT_FFI_FUNC(ReportPanic)(void** args, void* ret) noexcept;
}
