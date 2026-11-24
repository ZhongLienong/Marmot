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
	FunctionParameters m_params;
	std::vector<std::shared_ptr<MarmotType>> m_param_types;
	std::vector<MarmotType::ClassConstraint> m_constraints;
	std::vector<std::shared_ptr<MarmotType>> m_generic_param_types;
	std::shared_ptr<MarmotType> m_generic_return_type;
	std::shared_ptr<const MarmotExpression> m_body;
	std::string m_defining_module;

	GenericFunctionInfo() = default;

	GenericFunctionInfo(std::string name, FunctionParameters params, std::vector<std::shared_ptr<MarmotType>> param_types, const std::vector<Token>& generic_params, std::vector<MarmotType::ClassConstraint> constraints, std::shared_ptr<MarmotType> return_type, std::shared_ptr<const MarmotExpression> body, std::string defining_module)
		: m_name(std::move(name)),
		m_params(std::move(params)),
		m_param_types(std::move(param_types)),
		m_constraints(std::move(constraints)),
		m_generic_return_type(std::move(return_type)),
		m_body(std::move(body)),
		m_defining_module(std::move(defining_module))
	{
		for (const Token& t : generic_params)
		{
			m_generic_param_types.emplace_back(MarmotType::MakeGenericType(t.m_lexeme));
		}
	}
};

using GenericFunctions = std::unordered_map<std::string, GenericFunctionInfo>;
