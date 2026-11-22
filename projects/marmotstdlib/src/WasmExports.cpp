#ifdef __EMSCRIPTEN__

#include "MarmotStdLibExports.h"

#include <string_view>

MarmotFFIFn MarmotStdLibResolve(std::string_view symbol)
{
#define MARMOT_STDLIB_SYMBOL(name) if (symbol == "MARMOT_FFI_" #name) { return &MARMOT_FFI_FUNC(name); }
#include "MarmotStdLibExports.def"
#undef MARMOT_STDLIB_SYMBOL
	return nullptr;
}

#endif
