#pragma once

#include <optional>
#include <string_view>

// An operation the VM performs as an instruction, which Marmot declares as a
// `foreign` with no library: `foreign "MARMOT_PRIMITIVE_Spawn" Spawn: ...;`. The
// declaration is the only way the compiler knows one.
enum class MarmotPrimitive
{
	Spawn,
	Join,
	ChannelNew,
	ChannelClose,
	WorkerIsDone,
	WorkerCancel,
};

std::optional<MarmotPrimitive> FindPrimitive(std::string_view symbol);
