#include "Ordering.h"
#include "Compiler/Names/SpecialNames.h"
#include "Support/Optional.h"

namespace
{
	constexpr std::array<std::string_view, 4u> s_constructor_names{ "Less", "Equal", "Greater", "Unordered" };
}

MarmotCompiler::Optional<OrderingType> OrderingType::Resolve(const std::unordered_map<std::string, std::shared_ptr<MarmotType>>& methods)
{
	const auto compare = methods.find(std::string(COMPARE_METHOD_NAME));
	if (compare == methods.end())
	{
		return MarmotCompiler::None;
	}
	const std::shared_ptr<MarmotType>& result = compare->second->GetType<MarmotType::FunctionType>().m_return_type;
	if (!result->IsType<MarmotType::UnionType>())
	{
		return MarmotCompiler::None;
	}
	const MarmotType::UnionType& union_type = result->GetType<MarmotType::UnionType>();
	if (union_type.m_member_info.size() != s_constructor_names.size())
	{
		return MarmotCompiler::None;
	}
	OrderingType ordering{ result, {} };
	for (size_t index = 0u; index < s_constructor_names.size(); ++index)
	{
		const auto member = union_type.m_member_info.find(ordering.ConstructorName(static_cast<MarmotOrdering>(index)));
		if (member == union_type.m_member_info.end() || !member->second.m_member_types.empty())
		{
			return MarmotCompiler::None;
		}
		ordering.m_tags[index] = member->second.m_tag;
	}
	return ordering;
}

std::string OrderingType::ConstructorName(MarmotOrdering value) const
{
	return m_type->GetType<MarmotType::UnionType>().m_name + std::string(NameSeparator) + std::string(s_constructor_names[static_cast<size_t>(value)]);
}

int OrderingType::Tag(MarmotOrdering value) const
{
	return m_tags[static_cast<size_t>(value)];
}
