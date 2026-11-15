#include "Primitive.h"

#include <algorithm>
#include <array>
#include <utility>

namespace
{
	using PrimitiveSymbol = std::pair<std::string_view, MarmotPrimitive>;
}

std::optional<MarmotPrimitive> FindPrimitive(std::string_view symbol)
{
	static constexpr std::array<PrimitiveSymbol, 6u> s_primitives =
	{{
		{ "MARMOT_PRIMITIVE_Spawn", MarmotPrimitive::Spawn },
		{ "MARMOT_PRIMITIVE_Join", MarmotPrimitive::Join },
		{ "MARMOT_PRIMITIVE_ChannelNew", MarmotPrimitive::ChannelNew },
		{ "MARMOT_PRIMITIVE_ChannelClose", MarmotPrimitive::ChannelClose },
		{ "MARMOT_PRIMITIVE_WorkerIsDone", MarmotPrimitive::WorkerIsDone },
		{ "MARMOT_PRIMITIVE_WorkerCancel", MarmotPrimitive::WorkerCancel },
	}};

	const std::array<PrimitiveSymbol, 6u>::const_iterator found = std::ranges::find(s_primitives, symbol, &PrimitiveSymbol::first);
	return found != s_primitives.cend() ? std::optional<MarmotPrimitive>(found->second) : std::nullopt;
}
