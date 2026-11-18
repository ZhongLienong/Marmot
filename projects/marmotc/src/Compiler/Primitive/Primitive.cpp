#include "Primitive.h"
#include "Compiler/Operator/Operator.h"

#include <algorithm>
#include <array>
#include <utility>

namespace
{
	using PrimitiveSymbol = std::pair<std::string_view, MarmotPrimitive>;
}

std::optional<MarmotPrimitive> FindPrimitive(std::string_view symbol)
{
	constexpr std::string_view prefix = "MARMOT_PRIMITIVE_";
	if (symbol.starts_with(prefix))
	{
		for (const MarmotOperator& op : MarmotOperators())
		{
			if (symbol.substr(prefix.size()) == op.m_class_name)
			{
				return op.m_primitive;
			}
		}
	}
	static constexpr std::array<PrimitiveSymbol, 8u> s_primitives =
	{{
		{ "MARMOT_PRIMITIVE_Spawn", MarmotPrimitive::Spawn },
		{ "MARMOT_PRIMITIVE_Join", MarmotPrimitive::Join },
		{ "MARMOT_PRIMITIVE_ChannelNew", MarmotPrimitive::ChannelNew },
		{ "MARMOT_PRIMITIVE_ChannelClose", MarmotPrimitive::ChannelClose },
		{ "MARMOT_PRIMITIVE_ChannelSend", MarmotPrimitive::ChannelSend },
		{ "MARMOT_PRIMITIVE_ChannelReceive", MarmotPrimitive::ChannelReceive },
		{ "MARMOT_PRIMITIVE_WorkerIsDone", MarmotPrimitive::WorkerIsDone },
		{ "MARMOT_PRIMITIVE_WorkerCancel", MarmotPrimitive::WorkerCancel },
	}};

	const auto found = std::ranges::find(s_primitives, symbol, &PrimitiveSymbol::first);
	return found != s_primitives.cend() ? std::optional<MarmotPrimitive>(found->second) : std::nullopt;
}
