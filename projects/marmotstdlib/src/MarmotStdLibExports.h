#pragma once

#include "MarmotNativeAbi.h"

#define MARMOT_FFI_FUNC(name) MARMOT_FFI_##name

extern "C"
{
#define MARMOT_STDLIB_SYMBOL(name) MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(name)(void** args, void* ret) noexcept;
#include "MarmotStdLibExports.def"
#undef MARMOT_STDLIB_SYMBOL
}
