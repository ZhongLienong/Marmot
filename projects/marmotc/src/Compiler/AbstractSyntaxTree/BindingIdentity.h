#pragma once

#include <memory>

struct LexicalFunction {};
using LexicalFunctionId = std::shared_ptr<const LexicalFunction>;

// Shared by a declaration and every reference to it, including imported generic
// bodies. The index is a display label, never a storage location or lookup key.
struct BindingIdentity
{
	LexicalFunctionId m_function;
	int m_index;

	BindingIdentity(LexicalFunctionId function, int index);
};

using BindingId = std::shared_ptr<const BindingIdentity>;
