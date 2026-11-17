#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

class UnusedLocalDiagnostic final : public DiagnosticPass
{
protected:
	void Reset(MarmotProgramTree& program_tree) override;

	void Finish() override;

	void operator()(MarmotStatement::VariableDefinition& def) override;
	void operator()(MarmotStatement::TupleDefinition& def_tuple) override;
	void operator()(MarmotStatement::FunctionDefinition& defun) override;
	void operator()(MarmotStatement::ForeignDefinition& foreign) override;
	void operator()(MarmotPattern::Binding& binding) override;
	void operator()(MarmotExpression::NameAccess& access) override;
	void operator()(MarmotExpression::Function& function) override;
	void operator()(MarmotExpression::Block& block) override;
	void operator()(MarmotExpression::Case& case_expr) override;

private:
	struct BindingInfo
	{
		Token m_token;
		std::string m_display_name;
		bool m_is_read = false;
		bool m_is_suppressed = false;
	};

	std::unordered_map<BindingId, BindingInfo> m_bindings;
	struct Scope
	{
		Scope* m_parent;
		std::vector<BindingId> m_bindings;

		explicit Scope(Scope* parent);
	};

	Scope m_root{ nullptr };
	Scope* m_scope = &m_root;

	template<typename Visit>
	void InScope(Visit&& visit)
	{
		Scope scope(m_scope);
		m_scope = &scope;
		std::forward<Visit>(visit)();
		m_scope = scope.m_parent;
		FinishScope(scope);
	}

	void FinishScope(const Scope& scope);
	void RegisterBinding(const Token& token, const std::optional<BindingId>& binding, bool suppressed);
	void RegisterParameterBindings(const FunctionParameters& params);
	void MarkRead(const MarmotExpression::NameContext::Tag& name_ctx);
};
