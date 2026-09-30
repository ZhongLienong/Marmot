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

std::string_view ShadowingPolicyDiagnostic::GetName() const
{
	return "ShadowingPolicyDiagnostic";
}

void ShadowingPolicyDiagnostic::Reset()
{
	m_scopes.clear();
	PushScope();
}

void ShadowingPolicyDiagnostic::Finish()
{
	while (!m_scopes.empty())
	{
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::PushScope()
{
	m_scopes.emplace_back();
}

void ShadowingPolicyDiagnostic::PopScope()
{
	m_scopes.pop_back();
}

void ShadowingPolicyDiagnostic::DefineVariable(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scopes.back().m_variables.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineStruct(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scopes.back().m_structs.insert(token.m_lexeme);
	m_scopes.back().m_types.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineUnionConstructor(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scopes.back().m_union_constructors.insert(token.m_lexeme);
}

void ShadowingPolicyDiagnostic::DefineType(const Token& token)
{
	const std::vector<std::string_view> kinds = CollectShadowKinds(token.m_lexeme);
	if (!kinds.empty())
	{
		EmitShadowWarning(token, kinds);
	}

	m_scopes.back().m_types.insert(token.m_lexeme);
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

	for (std::size_t index = m_scopes.size(); index > 1u; index -= 1u)
	{
		const Scope& scope = m_scopes[index - 2u];
		shadows_struct = shadows_struct || scope.m_structs.contains(mangled_name);
		shadows_variable = shadows_variable || scope.m_variables.contains(mangled_name);
		shadows_union = shadows_union || scope.m_union_constructors.contains(mangled_name);
		shadows_type = shadows_type || scope.m_types.contains(mangled_name);
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
	const std::string display_name = MidoriAnalysis::DemangleDisplayName(token.m_lexeme);
	EmitWarning
	(
		CompilerWarningCode::NameShadowing,
		token,
		std::format("Name '{}' shadows {} from an outer scope.", display_name, JoinKinds(kinds))
	);
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::VariableDefinition& def)
{
	DefineVariable(def.m_name);
	VisitExpression(def.m_value);
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::TupleDefinition& def_tuple)
{
	for (const Token& token : def_tuple.m_names)
	{
		DefineVariable(token);
	}

	VisitExpression(def_tuple.m_value);
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::FunctionDefinition& defun)
{
	DefineVariable(defun.m_name);

	if (!defun.m_generic_params.empty())
	{
		PushScope();
		DefineGenericParams(defun.m_generic_params);
	}

	PushScope();
	for (const Token& token : defun.m_params)
	{
		DefineVariable(token);
	}
	VisitExpression(defun.m_body);
	PopScope();

	if (!defun.m_generic_params.empty())
	{
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::ForeignDefinition& foreign)
{
	DefineVariable(foreign.m_function_name);
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::Struct& struct_stmt)
{
	DefineStruct(struct_stmt.m_name);

	if (!struct_stmt.m_generic_params.empty())
	{
		PushScope();
		DefineGenericParams(struct_stmt.m_generic_params);
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::Union& union_stmt)
{
	DefineType(union_stmt.m_name);

	if (!union_stmt.m_generic_params.empty())
	{
		PushScope();
		DefineGenericParams(union_stmt.m_generic_params);
	}

	for (const Token& constructor_name : union_stmt.m_constructor_names)
	{
		DefineUnionConstructor(constructor_name);
	}

	if (!union_stmt.m_generic_params.empty())
	{
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::operator()(MidoriStatement::TypeAlias& type_alias)
{
	DefineType(type_alias.m_name);

	if (!type_alias.m_generic_params.empty())
	{
		PushScope();
		DefineGenericParams(type_alias.m_generic_params);
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::operator()(MidoriPattern::Binding& binding)
{
	DefineVariable(binding.m_name);
}

void ShadowingPolicyDiagnostic::operator()(MidoriExpression::Function& function)
{
	if (!function.m_generic_params.empty())
	{
		PushScope();
		DefineGenericParams(function.m_generic_params);
	}

	PushScope();
	for (const Token& token : function.m_params)
	{
		DefineVariable(token);
	}
	VisitExpression(function.m_body);
	PopScope();

	if (!function.m_generic_params.empty())
	{
		PopScope();
	}
}

void ShadowingPolicyDiagnostic::operator()(MidoriExpression::Block& block)
{
	PushScope();

	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitExpression(block.m_final_expr.value());
	}

	PopScope();
}

void ShadowingPolicyDiagnostic::operator()(MidoriExpression::Case& case_expr)
{
	PushScope();
	VisitPattern(case_expr.m_pattern);
	if (case_expr.HasGuard())
	{
		VisitExpression(case_expr.m_guard.value());
	}
	VisitExpression(case_expr.m_expr);
	PopScope();
}
