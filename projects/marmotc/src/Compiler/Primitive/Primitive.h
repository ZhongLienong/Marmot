#pragma once

#include <string_view>

#include "Support/Optional.h"

// An operation the VM performs as an instruction, which Marmot declares as a
// `foreign` with no library: `foreign "MARMOT_PRIMITIVE_Spawn" Spawn: ...;`. The
// declaration is the only way the compiler knows one.
enum class MarmotPrimitive
{
	Spawn,
	Join,
	ChannelNew,
	ChannelClose,
	ChannelSend,
	ChannelReceive,
	WorkerIsDone,
	WorkerCancel,
#define MARMOT_BINARY_OPERATOR(token, name, integer, shift) name,
#define MARMOT_UNARY_OPERATOR(token, name, integer) name,
#include "Compiler/Operator/Operators.def"
#undef MARMOT_BINARY_OPERATOR
#undef MARMOT_UNARY_OPERATOR
};

MarmotCompiler::Optional<MarmotPrimitive> FindPrimitive(std::string_view symbol);
