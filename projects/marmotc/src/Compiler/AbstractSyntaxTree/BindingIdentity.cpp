#include "BindingIdentity.h"

#include <utility>

BindingIdentity::BindingIdentity(LexicalFunctionId function, int index)
	: m_function(std::move(function)),
	m_index(index)
{
}
