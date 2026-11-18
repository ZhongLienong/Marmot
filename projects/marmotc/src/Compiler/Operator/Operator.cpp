#include "Operator.h"

#include <algorithm>
#include <format>

std::string MarmotOperator::MethodName() const
{
	return std::format("{0}::{0}", m_class_name);
}

std::span<const MarmotOperator> MarmotOperators()
{
	static constexpr MarmotOperator s_operators[] =
	{
#define MARMOT_BINARY_OPERATOR(token, name, integer, shift) { Token::Name::token, #name, MarmotPrimitive::name, false, integer, shift },
#define MARMOT_UNARY_OPERATOR(token, name, integer) { Token::Name::token, #name, MarmotPrimitive::name, true, integer, false },
#include "Operators.def"
#undef MARMOT_BINARY_OPERATOR
#undef MARMOT_UNARY_OPERATOR
	};
	return s_operators;
}

const MarmotOperator* FindOperator(Token::Name token, bool unary)
{
	const std::span<const MarmotOperator> operators = MarmotOperators();
	const std::span<const MarmotOperator>::iterator found = std::ranges::find_if(operators, [=](const MarmotOperator& op) { return op.m_token == token && op.m_unary == unary; });
	return found == operators.end() ? nullptr : &*found;
}

const MarmotOperator* PrimitiveOperator(MarmotPrimitive primitive)
{
	const std::span<const MarmotOperator> operators = MarmotOperators();
	const std::span<const MarmotOperator>::iterator found = std::ranges::find(operators, primitive, &MarmotOperator::m_primitive);
	return found == operators.end() ? nullptr : &*found;
}
