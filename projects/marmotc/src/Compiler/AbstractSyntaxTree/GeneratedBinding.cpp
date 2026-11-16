#include "GeneratedBinding.h"

#include <format>

GeneratedBinding::GeneratedBinding(int index, const Token& anchor)
	: m_index(index), m_anchor(anchor)
{
}

std::string GeneratedBinding::DisplayName() const
{
	return std::format("%local{}", m_index);
}
