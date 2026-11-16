#pragma once

#include "Compiler/Token/Token.h"

#include <string>

// A compiler-generated local, identified by its index within a function.
// For example, deriving Equatable creates parameters for the two compared
// values; deriving for unions also creates bindings for matched fields.
// The anchor points diagnostics at the source declaration that generated it.
struct GeneratedBinding
{
	int m_index;
	Token m_anchor;

	GeneratedBinding(int index, const Token& anchor);
	// A display label only; binding resolution uses m_index.
	std::string DisplayName() const;
};
