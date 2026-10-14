#include "Primitive.h"

#include <algorithm>
#include <array>
#include <utility>

namespace
{
	using PrimitiveSymbol = std::pair<std::string_view, MidoriPrimitive>;
}

std::optional<MidoriPrimitive> FindPrimitive(std::string_view symbol)
{
	static constexpr std::array<PrimitiveSymbol, 6u> s_primitives =
	{{
		{ "MIDORI_PRIMITIVE_Spawn", MidoriPrimitive::Spawn },
		{ "MIDORI_PRIMITIVE_Join", MidoriPrimitive::Join },
		{ "MIDORI_PRIMITIVE_ChannelNew", MidoriPrimitive::ChannelNew },
		{ "MIDORI_PRIMITIVE_ChannelClose", MidoriPrimitive::ChannelClose },
		{ "MIDORI_PRIMITIVE_WorkerIsDone", MidoriPrimitive::WorkerIsDone },
		{ "MIDORI_PRIMITIVE_WorkerCancel", MidoriPrimitive::WorkerCancel },
	}};

	const std::array<PrimitiveSymbol, 6u>::const_iterator found = std::ranges::find(s_primitives, symbol, &PrimitiveSymbol::first);
	return found != s_primitives.cend() ? std::optional<MidoriPrimitive>(found->second) : std::nullopt;
}
