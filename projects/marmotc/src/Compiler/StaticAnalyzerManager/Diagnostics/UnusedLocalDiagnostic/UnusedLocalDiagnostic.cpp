#include "UnusedLocalDiagnostic.h"

#include "Compiler/Analysis/SharedAnalysis.h"
#include "Support/Optional.h"

#include <format>

void UnusedLocalDiagnostic::Finish()
{
	FinishScope(m_root);
}

void UnusedLocalDiagnostic::FinishScope(const std::vector<BindingId>& scope)
{
	for (const BindingId& identity : scope)
	{
		const auto entry = m_bindings.find(identity);
		const BindingInfo& binding = entry->second;
		if (!binding.m_is_suppressed && !binding.m_is_read)
		{
			const std::string name = MarmotAnalysis::DemangleDisplayName(binding.m_token.m_lexeme);
			if (!MarmotAnalysis::IsIgnoredBindingName(name))
			{
				EmitWarning(CompilerWarningCode::UnusedLocal, binding.m_token, std::format("Binding '{}' is never read.", name));
			}
		}
		m_bindings.erase(entry);
	}
}

void UnusedLocalDiagnostic::RegisterBinding(const Token& token, const MarmotCompiler::Optional<BindingId>& binding, bool suppressed)
{
	if (!binding.HasValue() || !binding.Value())
	{
		return;
	}
	const bool inserted = m_bindings.try_emplace(binding.Value(), BindingInfo{ token, suppressed }).second;
	if (inserted)
	{
		m_scope->push_back(binding.Value());
	}
}

void UnusedLocalDiagnostic::RegisterParameterBindings(const FunctionParameters& params)
{
	for (size_t index = 0u; index < params.Size(); index += 1u)
	{
		RegisterBinding(params.SourceToken(index), params.BindingAt(index), params.GeneratedBindingAt(index) != nullptr);
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
	RegisterBinding(def.m_name, def.m_binding, def.m_is_elided);
	VisitExpression(def.m_value);
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::TupleDefinition& def_tuple)
{
	for (std::size_t index = 0u; index < def_tuple.m_names.size(); index += 1u)
	{
		RegisterBinding(def_tuple.m_names[index], def_tuple.m_bindings[index]);
	}

	VisitExpression(def_tuple.m_value);
}

void UnusedLocalDiagnostic::operator()(MarmotStatement::FunctionDefinition& defun)
{
	RegisterBinding(defun.m_name, defun.m_binding);

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
	RegisterBinding(foreign.m_function_name, foreign.m_binding);
}

void UnusedLocalDiagnostic::operator()(MarmotPattern::Binding& binding)
{
	RegisterBinding(binding.m_name, binding.m_binding, binding.m_is_generated);
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
	InScope([&]() { MarmotAbstractSyntaxTreeWalker::operator()(block); });
}

void UnusedLocalDiagnostic::operator()(MarmotExpression::Case& case_expr)
{
	InScope([&]() { MarmotAbstractSyntaxTreeWalker::operator()(case_expr); });
}
