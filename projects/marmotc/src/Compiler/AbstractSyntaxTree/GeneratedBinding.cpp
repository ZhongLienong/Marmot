#include "GeneratedBinding.h"

#include <format>

GeneratedBinding::GeneratedBinding(int index, const Token& anchor, LexicalFunctionId function)
	: m_binding(std::make_shared<BindingIdentity>(std::move(function), index)), m_anchor(anchor)
{
}

std::string GeneratedBinding::DisplayName() const
{
	return std::format("%local{}", m_binding->m_index);
}
