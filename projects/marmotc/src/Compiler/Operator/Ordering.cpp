#include "Ordering.h"

std::shared_ptr<MarmotType> MarmotOrderingType()
{
	std::shared_ptr<MarmotType> type = MarmotType::MakeUnionType("Ordering", "Orderable");
	MarmotType::UnionType& ordering = type->GetType<MarmotType::UnionType>();
	ordering.m_member_info.emplace("Ordering::Less", MarmotType::UnionType::UnionMemberContext{ {}, static_cast<int>(MarmotOrdering::Less) });
	ordering.m_member_info.emplace("Ordering::Equal", MarmotType::UnionType::UnionMemberContext{ {}, static_cast<int>(MarmotOrdering::Equal) });
	ordering.m_member_info.emplace("Ordering::Greater", MarmotType::UnionType::UnionMemberContext{ {}, static_cast<int>(MarmotOrdering::Greater) });
	ordering.m_member_info.emplace("Ordering::Unordered", MarmotType::UnionType::UnionMemberContext{ {}, static_cast<int>(MarmotOrdering::Unordered) });
	return type;
}
