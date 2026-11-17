#pragma once

#include "Compiler/Token/Token.h"
#include "Compiler/AbstractSyntaxTree/BindingIdentity.h"

#include <string>

// A compiler-generated local with the same identity as a source binding.
// For example, deriving Equatable creates parameters for the two compared
// values; deriving for unions also creates bindings for matched fields.
// The anchor points diagnostics at the source declaration that generated it.
struct GeneratedBinding
{
	BindingId m_binding;
	Token m_anchor;

	GeneratedBinding(int index, const Token& anchor, LexicalFunctionId function);
	// A display label only; binding resolution uses m_binding.
	std::string DisplayName() const;
};
