#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Compiler/Token/Token.h"
#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

struct GenericFunctionInfo
{
	std::string m_name;
	std::vector<Token> m_params;
	std::vector<std::shared_ptr<MidoriType>> m_param_types;
	std::vector<MidoriType::ClassConstraint> m_constraints;
	std::vector<std::shared_ptr<MidoriType>> m_generic_param_types;
	std::shared_ptr<MidoriType> m_generic_return_type;
	std::shared_ptr<MidoriExpression> m_body;
	std::string m_defining_module;
	// The defining module's builtin foreign functions, by the name its source
	// calls them. Shared with that module's code generator, which fills it as
	// it reaches each declaration: a foreign may come after the generic.
	std::shared_ptr<const std::unordered_map<std::string, size_t>> m_builtin_foreign_indices;
	int m_captured_count;

	GenericFunctionInfo() = default;

	GenericFunctionInfo(std::string name, std::vector<Token> params, std::vector<std::shared_ptr<MidoriType>> param_types, const std::vector<Token>& generic_params, std::vector<MidoriType::ClassConstraint> constraints, std::shared_ptr<MidoriType> return_type, std::shared_ptr<MidoriExpression> body, int captured_count, std::string defining_module, std::shared_ptr<const std::unordered_map<std::string, size_t>> builtin_foreign_indices)
		: m_name(std::move(name)),
		m_params(std::move(params)),
		m_param_types(std::move(param_types)),
		m_constraints(std::move(constraints)),
		m_generic_return_type(std::move(return_type)),
		m_body(std::move(body)),
		m_defining_module(std::move(defining_module)),
		m_builtin_foreign_indices(std::move(builtin_foreign_indices)),
		m_captured_count(captured_count)
	{
		for (const Token& t : generic_params)
		{
			m_generic_param_types.emplace_back(MidoriType::MakeGenericType(t.m_lexeme));
		}
	}
};
