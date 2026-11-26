#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"
#include "Support/Optional.h"

#include <unordered_map>
#include <vector>
#include <utility>

class UnusedLocalDiagnostic final : public DiagnosticPass
{
protected:
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
		const Token& m_token;
		bool m_is_suppressed;
		bool m_is_read = false;
	};

	std::unordered_map<BindingId, BindingInfo> m_bindings;
	std::vector<BindingId> m_root;
	std::vector<BindingId>* m_scope = &m_root;

	template<typename Visit>
	void InScope(Visit&& visit)
	{
		std::vector<BindingId> scope;
		std::vector<BindingId>* parent = std::exchange(m_scope, &scope);
		std::forward<Visit>(visit)();
		m_scope = parent;
		FinishScope(scope);
	}

	void FinishScope(const std::vector<BindingId>& scope);
	void RegisterBinding(const Token& token, const MarmotCompiler::Optional<BindingId>& binding, bool suppressed = false);
	void RegisterParameterBindings(const FunctionParameters& params);
	void MarkRead(const MarmotExpression::NameContext::Tag& name_ctx);
};
