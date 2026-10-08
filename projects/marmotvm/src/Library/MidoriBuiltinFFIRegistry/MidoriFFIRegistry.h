#pragma once

#include "Bytecode/Builtins/BuiltinTable.h"
#include "Library/MidoriStdLibExports.h"

#include <array>

// The runtime half of the builtin table: the function each VmBuiltins entry
// calls, expanded from the same Builtins.def.
inline constexpr std::array BUILTIN_FUNCTIONS =
{
#define MARMOT_BUILTIN(name, arg_kinds, return_kind) VmFFIFunction{ &name },
#include "Bytecode/Builtins/Builtins.def"
#undef MARMOT_BUILTIN
};

static_assert(BUILTIN_FUNCTIONS.size() == VmBuiltins::COUNT, "the runtime and shared builtin tables must list the same entries");
