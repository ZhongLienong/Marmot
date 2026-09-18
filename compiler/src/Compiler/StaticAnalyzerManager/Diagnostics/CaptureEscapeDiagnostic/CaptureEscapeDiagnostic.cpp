#include "CaptureEscapeDiagnostic.h"

#include "Compiler/Analysis/SharedAnalysis.h"

#include <format>

std::string_view CaptureEscapeDiagnostic::GetName() const
{
	return "CaptureEscapeDiagnostic";
}

void CaptureEscapeDiagnostic::Reset()
{
	m_functions.clear();
	PushFunctionContext();
}

void CaptureEscapeDiagnostic::Finish()
{
	while (!m_functions.empty())
	{
		PopFunctionContext();
	}
}

void CaptureEscapeDiagnostic::PushFunctionContext()
{
	m_functions.emplace_back();
	m_functions.back().m_scopes.emplace_back();
}

void CaptureEscapeDiagnostic::PopFunctionContext()
{
	m_functions.pop_back();
}

void CaptureEscapeDiagnostic::PushScope()
{
	m_functions.back().m_scopes.emplace_back();
}

void CaptureEscapeDiagnostic::PopScope()
{
	m_functions.back().m_scopes.pop_back();
}

void CaptureEscapeDiagnostic::RegisterClosureBinding(std::optional<int> local_index, const Token& token, bool has_captures)
{
	if (!has_captures || !local_index.has_value())
	{
		return;
	}

	m_functions.back().m_scopes.back().m_closures.emplace
	(
		local_index.value(),
		ClosureBinding
		{
			token,
			MidoriAnalysis::DemangleDisplayName(token.m_lexeme)
		}
	);
}

const CaptureEscapeDiagnostic::ClosureBinding* CaptureEscapeDiagnostic::ResolveClosure(const MidoriExpression::NameContext::Tag& name_ctx) const
{
	if (std::holds_alternative<MidoriExpression::NameContext::Local>(name_ctx))
	{
		const int local_index = std::get<MidoriExpression::NameContext::Local>(name_ctx).m_index;
		for (std::size_t scope_index = m_functions.back().m_scopes.size(); scope_index > 0u; scope_index -= 1u)
		{
			const Scope& scope = m_functions.back().m_scopes[scope_index - 1u];
			const std::unordered_map<int, ClosureBinding>::const_iterator it = scope.m_closures.find(local_index);
			if (it != scope.m_closures.end())
			{
				return &it->second;
			}
		}
		return nullptr;
	}

	if (!std::holds_alternative<MidoriExpression::NameContext::Cell>(name_ctx))
	{
		return nullptr;
	}

	const int local_index = std::get<MidoriExpression::NameContext::Cell>(name_ctx).m_index;
	for (std::size_t function_index = m_functions.size(); function_index > 1u; function_index -= 1u)
	{
		const FunctionContext& function = m_functions[function_index - 2u];
		for (std::size_t scope_index = function.m_scopes.size(); scope_index > 0u; scope_index -= 1u)
		{
			const Scope& scope = function.m_scopes[scope_index - 1u];
			const std::unordered_map<int, ClosureBinding>::const_iterator it = scope.m_closures.find(local_index);
			if (it != scope.m_closures.end())
			{
				return &it->second;
			}
		}
	}

	return nullptr;
}

std::optional<CaptureEscapeDiagnostic::EscapingClosure> CaptureEscapeDiagnostic::TryGetEscapingClosure(const MidoriExpression& expression) const
{
	const MidoriExpression* stripped = MidoriAnalysis::StripRedundantGroups(&expression);
	if (stripped == nullptr)
	{
		return std::nullopt;
	}

	if (stripped->IsExpression<MidoriExpression::Function>())
	{
		const MidoriExpression::Function& function = stripped->GetExpression<MidoriExpression::Function>();
		if (function.m_captured_count > 0)
		{
			return EscapingClosure
			{
				function.m_function_keyword,
				"anonymous closure"
			};
		}
		return std::nullopt;
	}

	if (stripped->IsExpression<MidoriExpression::NameAccess>())
	{
		const MidoriExpression::NameAccess& access = stripped->GetExpression<MidoriExpression::NameAccess>();
		const ClosureBinding* binding = ResolveClosure(access.m_name_ctx);
		if (binding != nullptr)
		{
			return EscapingClosure
			{
				access.m_name,
				binding->m_display_name
			};
		}
		return std::nullopt;
	}

	if (stripped->IsExpression<MidoriExpression::Block>())
	{
		const MidoriExpression::Block& block = stripped->GetExpression<MidoriExpression::Block>();
		if (block.m_final_expr.has_value())
		{
			return TryGetEscapingClosure(*block.m_final_expr.value());
		}
		return std::nullopt;
	}

	return std::nullopt;
}

void CaptureEscapeDiagnostic::WarnOnEscapingClosure(const MidoriExpression& expression)
{
	const std::optional<EscapingClosure> escaping = TryGetEscapingClosure(expression);
	if (!escaping.has_value())
	{
		return;
	}

	if (escaping->m_display_name == "anonymous closure")
	{
		EmitWarning
		(
			CompilerWarningCode::CaptureEscape,
			escaping->m_token,
			"Captured anonymous closure escapes its defining scope as a return value."
		);
		return;
	}

	EmitWarning
	(
		CompilerWarningCode::CaptureEscape,
		escaping->m_token,
		std::format("Captured closure '{}' escapes its defining scope as a return value.", escaping->m_display_name)
	);
}

void CaptureEscapeDiagnostic::operator()(MidoriStatement::VariableDefinition& def)
{
	bool has_capturing_closure = false;
	const MidoriExpression* stripped = MidoriAnalysis::StripRedundantGroups(def.m_value.get());
	if (stripped != nullptr && stripped->IsExpression<MidoriExpression::Function>())
	{
		has_capturing_closure = stripped->GetExpression<MidoriExpression::Function>().m_captured_count > 0;
	}

	RegisterClosureBinding(def.m_local_index, def.m_name, has_capturing_closure);
	VisitExpression(def.m_value);
}

void CaptureEscapeDiagnostic::operator()(MidoriStatement::FunctionDefinition& defun)
{
	RegisterClosureBinding(defun.m_local_index, defun.m_name, defun.m_captured_count > 0);

	PushFunctionContext();
	VisitFunctionBody(defun.m_body);
	PopFunctionContext();
}

void CaptureEscapeDiagnostic::operator()(MidoriExpression::Function& function)
{
	PushFunctionContext();
	VisitFunctionBody(function.m_body);
	PopFunctionContext();
}

void CaptureEscapeDiagnostic::VisitFunctionBody(std::unique_ptr<MidoriExpression>& body)
{
	const MidoriExpression::Block* saved_result_block = m_result_block;
	const MidoriExpression* stripped = MidoriAnalysis::StripRedundantGroups(body.get());
	const bool body_is_block = stripped != nullptr && stripped->IsExpression<MidoriExpression::Block>();
	m_result_block = body_is_block ? &stripped->GetExpression<MidoriExpression::Block>() : nullptr;

	VisitExpression(body);

	// A block body reports from inside the block (see operator()(Block&)), where
	// a closure bound in it is still in scope; by now that scope has been popped.
	if (!body_is_block)
	{
		WarnOnEscapingClosure(*body);
	}

	m_result_block = saved_result_block;
}

void CaptureEscapeDiagnostic::operator()(MidoriExpression::Block& block)
{
	PushScope();

	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitExpression(block.m_final_expr.value());
		if (&block == m_result_block)
		{
			WarnOnEscapingClosure(*block.m_final_expr.value());
		}
	}

	PopScope();
}

