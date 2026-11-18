#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

enum class MarmotOrdering
{
	Less,
	Equal,
	Greater,
	Unordered
};

std::shared_ptr<MarmotType> MarmotOrderingType();
