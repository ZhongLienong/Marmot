#include "ShadowingPolicyDiagnostic.h"

#include "Compiler/Analysis/SharedAnalysis.h"

#include <format>

namespace
{
	std::string JoinKinds(const std::vector<std::string_view>& kinds)
	{
		if (kinds.empty())
		{
			return {};
		}

		if (kinds.size() == 1u)
		{
			return std::string(kinds.front());
		}

		if (kinds.size() == 2u)
		{
			return std::format("{} and {}", kinds[0], kinds[1]);
		}

		std::string result;
		for (std::size_t index = 0u; index < kinds.size(); index += 1u)
		{
			if (index > 0u)
			{
				result.append(index + 1u == kinds.size() ? ", and " : ", ");
			}
			result.append(kinds[index]);
		}
		return result;
	}
}

ShadowingPolicyDiagnostic::Scope::Scope(Scope* parent)
	: m_parent(parent)
{
}

void ShadowingPolicyDiagnostic::DefineVariable(const Token& token)
{
	if (token.IsDiscard())
	{
		return;
	}
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scope->m_variables.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineStruct(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scope->m_structs.insert(token.m_lexeme);
	m_scope->m_types.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineUnionConstructor(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scope->m_union_constructors.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineType(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scope->m_types.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineGenericParams(const std::vector<Token>& generic_params)
{
	for (const Token& token : generic_params)
	{
		DefineType(token);
	}
}

std::vector<std::string_view> ShadowingPolicyDiagnostic::CollectShadowKinds(const std::string& mangled_name) const
{
	bool shadows_struct = false;
	bool shadows_variable = false;
	bool shadows_union = false;
	bool shadows_type = false;

	for (const Scope* scope = m_scope->m_parent; scope != nullptr; scope = scope->m_parent)
	{
		shadows_struct = shadows_struct || scope->m_structs.contains(mangled_name);
		shadows_variable = shadows_variable || scope->m_variables.contains(mangled_name);
		shadows_union = shadows_union || scope->m_union_constructors.contains(mangled_name);
		shadows_type = shadows_type || scope->m_types.contains(mangled_name);
	}

	std::vector<std::string_view> kinds;
	if (shadows_struct)
	{
		kinds.push_back("a struct");
	}
	if (shadows_variable)
	{
		kinds.push_back("a variable");
	}
	if (shadows_union)
	{
		kinds.push_back("a union constructor");
	}
	if (shadows_type)
	{
		kinds.push_back("a type");
	}
	return kinds;
}

void ShadowingPolicyDiagnostic::EmitShadowWarning(const Token& token, const std::vector<std::string_view>& kinds)
{
	const std::string display_name = MarmotAnalysis::DemangleDisplayName(token.m_lexeme);
	EmitWarning
	(
		CompilerWarningCode::NameShadowing,
		token,
		std::format("Name '{}' shadows {} from an outer scope.", display_name, JoinKinds(kinds))
	);
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::VariableDefinition& def)
{
	DefineVariable(def.m_name);
	VisitExpression(def.m_value);
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::TupleDefinition& def_tuple)
{
	for (const Token& token : def_tuple.m_names)
	{
		DefineVariable(token);
	}

	VisitExpression(def_tuple.m_value);
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::FunctionDefinition& defun)
{
	DefineVariable(defun.m_name);

	WithGenericParameters
	(
		defun.m_generic_params,
		[&]()
		{
			InScope
			(
				[&]()
				{
					for (size_t index = 0u; index < defun.m_params.Size(); index += 1u)
					{
						if (defun.m_params.GeneratedBindingAt(index) == nullptr)
						{
							DefineVariable(defun.m_params.SourceToken(index));
						}
					}
					VisitExpression(defun.m_body);
				}
			);
		}
	);
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::ForeignDefinition& foreign)
{
	DefineVariable(foreign.m_function_name);
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::Struct& struct_stmt)
{
	DefineStruct(struct_stmt.m_name);

	WithGenericParameters(struct_stmt.m_generic_params, []() {});
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::Union& union_stmt)
{
	DefineType(union_stmt.m_name);

	WithGenericParameters(union_stmt.m_generic_params, [&]()
	{
		for (const Token& constructor_name : union_stmt.m_constructor_names)
		{
			DefineUnionConstructor(constructor_name);
		}
	});
}

void ShadowingPolicyDiagnostic::operator()(MarmotStatement::TypeAlias& type_alias)
{
	DefineType(type_alias.m_name);

	WithGenericParameters(type_alias.m_generic_params, []() {});
}

void ShadowingPolicyDiagnostic::operator()(MarmotPattern::Binding& binding)
{
	if (!binding.m_is_generated)
	{
		DefineVariable(binding.m_name);
	}
}

void ShadowingPolicyDiagnostic::operator()(MarmotExpression::Function& function)
{
	WithGenericParameters
	(
		function.m_generic_params,
		[&]()
		{
			InScope
			(
				[&]()
				{
					for (size_t index = 0u; index < function.m_params.Size(); index += 1u)
					{
						if (function.m_params.GeneratedBindingAt(index) == nullptr)
						{
							DefineVariable(function.m_params.SourceToken(index));
						}
					}
					VisitExpression(function.m_body);
				}
			);
		}
	);
}

void ShadowingPolicyDiagnostic::operator()(MarmotExpression::Block& block)
{
	InScope
	(
		[&]()
		{
			for (std::unique_ptr<MarmotStatement>& statement : block.m_stmts)
			{
				VisitStatement(statement);
			}

			if (block.m_final_expr.HasValue())
			{
				VisitExpression(block.m_final_expr.Value());
			}
		}
	);
}

void ShadowingPolicyDiagnostic::operator()(MarmotExpression::Case& case_expr)
{
	InScope
	(
		[&]()
		{
			VisitPattern(case_expr.m_pattern);
			if (case_expr.HasGuard())
			{
				VisitExpression(case_expr.m_guard.Value());
			}
			VisitExpression(case_expr.m_expr);
		}
	);
}
