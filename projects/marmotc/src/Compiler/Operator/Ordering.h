#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Support/Optional.h"

#include <array>

enum class MarmotOrdering
{
	Less,
	Equal,
	Greater,
	Unordered
};

struct OrderingType
{
	std::shared_ptr<MarmotType> m_type;
	std::array<int, 4u> m_tags;

	static constexpr std::string_view s_required_result = "Orderable::Compare must return a union with exactly the payload-free constructors Less, Equal, Greater, and Unordered.";

	static MarmotCompiler::Optional<OrderingType> Resolve(const std::unordered_map<std::string, std::shared_ptr<MarmotType>>& methods);
	std::string ConstructorName(MarmotOrdering value) const;
	int Tag(MarmotOrdering value) const;
};
