#pragma once

#include "Compiler/Primitive/Primitive.h"
#include "Compiler/Token/Token.h"

#include <span>
#include <string>
#include <string_view>

struct MarmotOperator
{
	Token::Name m_token;
	std::string_view m_class_name;
	MarmotPrimitive m_primitive;
	bool m_unary;
	bool m_integer_only;
	bool m_shift;

	std::string MethodName() const;
};

std::span<const MarmotOperator> MarmotOperators();
const MarmotOperator* FindOperator(Token::Name token, bool unary);
const MarmotOperator* PrimitiveOperator(MarmotPrimitive primitive);
