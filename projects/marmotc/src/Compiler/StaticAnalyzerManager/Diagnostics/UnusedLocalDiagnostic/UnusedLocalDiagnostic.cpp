#include "UnusedLocalDiagnostic.h"

#include "Compiler/Analysis/SharedAnalysis.h"

#include <format>

void UnusedLocalDiagnostic::Reset(MarmotProgramTree&)
{
	m_bindings.clear();
	m_root = Scope(nullptr);
	m_scope = &m_root;
}

void UnusedLocalDiagnostic::Finish()
{
	FinishScope(m_root);
}

UnusedLocalDiagnostic::Scope::Scope(Scope* parent)
	: m_parent(parent)
{
}

void UnusedLocalDiagnostic::FinishScope(const Scope& scope)
{
	for (const BindingId& identity : scope.m_bindings)
	{
		const BindingInfo& binding = m_bindings.at(identity);
		if (!binding.m_is_suppressed && !binding.m_is_read)
		{
			EmitWarning(CompilerWarningCode::UnusedLocal, binding.m_token, std::format("Binding '{}' is never read.", binding.m_display_name));
		}
		m_bindings.erase(identity);
	}
}

void UnusedLocalDiagnostic::RegisterBinding(const Token& token, const std::optional<BindingId>& binding, bool suppressed)
{
	if (!binding.has_value() || !binding.value())
	{
		return;
	}
	const bool inserted = m_bindings.emplace(binding.value(), BindingInfo{ token, MarmotAnalysis::DemangleDisplayName(token.m_lexeme), false, suppressed }).second;
	if (inserted)
	{
		m_scope->m_bindings.push_back(binding.value());
	}
}

void UnusedLocalDiagnostic::RegisterParameterBindings(const FunctionParameters& params)
{
	for (size_t index = 0u; index < params.Size(); index += 1u)
	{
		const Token& token = params.SourceToken(index);
		RegisterBinding(token, params.BindingAt(index), params.GeneratedBindingAt(index) != nullptr || MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(token.m_lexeme)));
	}
}

void UnusedLocalDiagnostic::MarkRead(const MarmotExpression::NameContext::Tag& name_ctx)
{
	const BindingId* binding = nullptr;
	if (const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&name_ctx))
	{
		binding = &local->m_binding;
	}
	else if (const MarmotExpression::NameContext::Cell* captured = std::get_if<MarmotExpression::NameContext::Cell>(&name_ctx))
	{
		binding = &captured->m_binding;
	}
	if (binding != nullptr)
	{
		const std::unordered_map<BindingId, BindingInfo>::iterator found = m_bindings.find(*binding);
		if (found != m_bindings.end())
		{
			found->second.m_is_read = true;
		}
	}
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::VariableDefinition& def)
{
	RegisterBinding(def.m_name, def.m_binding, def.m_is_elided || MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(def.m_name.m_lexeme)));
	VisitExpression(def.m_value);
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::TupleDefinition& def_tuple)
{
	for (std::size_t index = 0u; index < def_tuple.m_names.size(); index += 1u)
	{
		const Token& token = def_tuple.m_names[index];
		const std::optional<BindingId>& binding = def_tuple.m_bindings[index];
		RegisterBinding(token, binding, MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(token.m_lexeme)));
	}

	VisitExpression(def_tuple.m_value);
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::FunctionDefinition& defun)
{
	RegisterBinding(defun.m_name, defun.m_binding, MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(defun.m_name.m_lexeme)));

	InScope
	(
		[&]()
		{
			RegisterParameterBindings(defun.m_params);
			VisitExpression(defun.m_body);
		}
	);
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::ForeignDefinition& foreign)
{
	RegisterBinding(foreign.m_function_name, foreign.m_binding, MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(foreign.m_function_name.m_lexeme)));
}

void UnusedLocalDiagnostic::operator()(MarmotPattern::Binding& binding)
{
	RegisterBinding(binding.m_name, binding.m_binding, binding.m_is_generated || MarmotAnalysis::IsIgnoredBindingName(MarmotAnalysis::DemangleDisplayName(binding.m_name.m_lexeme)));
}

void UnusedLocalDiagnostic::operator()(MarmotExpression::NameAccess& access)
{
	MarkRead(access.m_name_ctx);
}

void UnusedLocalDiagnostic::operator()(MarmotExpression::Function& function)
{
	InScope
	(
		[&]()
		{
			RegisterParameterBindings(function.m_params);
			VisitExpression(function.m_body);
		}
	);
}

void UnusedLocalDiagnostic::operator()(MarmotExpression::Block& block)
{
	InScope
	(
		[&]()
		{
			for (std::unique_ptr<MarmotStatement>& statement : block.m_stmts)
			{
				VisitStatement(statement);
			}

			if (block.m_final_expr.has_value())
			{
				VisitExpression(block.m_final_expr.value());
			}
		}
	);
}

void UnusedLocalDiagnostic::operator()(MarmotExpression::Case& case_expr)
{
	InScope
	(
		[&]()
		{
			VisitPattern(case_expr.m_pattern);
			if (case_expr.HasGuard())
			{
				VisitExpression(case_expr.m_guard.value());
			}
			VisitExpression(case_expr.m_expr);
		}
	);
}
