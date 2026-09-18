#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class UnusedLocalDiagnostic final : public AstDiagnosticPass
{
public:
	std::string_view GetName() const override;

protected:
	void Reset() override;

	void Finish() override;

	void operator()(MidoriStatement::VariableDefinition& def) override;
	void operator()(MidoriStatement::TupleDefinition& def_tuple) override;
	void operator()(MidoriStatement::FunctionDefinition& defun) override;
	void operator()(MidoriStatement::ForeignDefinition& foreign) override;
	void operator()(MidoriPattern::Binding& binding) override;
	void operator()(MidoriExpression::NameAccess& access) override;
	void operator()(MidoriExpression::Function& function) override;
	void operator()(MidoriExpression::Block& block) override;
	void operator()(MidoriExpression::Case& case_expr) override;

private:
	struct BindingInfo
	{
		Token m_token;
		std::string m_display_name;
		bool m_is_read = false;
		bool m_is_suppressed = false;
	};

	struct Scope
	{
		std::vector<int> m_binding_ids;
	};

	struct FunctionContext
	{
		std::vector<Scope> m_scopes;
		std::unordered_map<int, int> m_active_locals;
		std::vector<BindingInfo> m_bindings;
	};

	void PushFunctionContext();
	void PopFunctionContext();
	void PushScope();
	void PopScope();
	void RegisterBinding(const Token& token, std::optional<int> local_index, bool suppressed);
	void RegisterParameterBindings(const std::vector<Token>& params);
	void MarkRead(const Token& name, const MidoriExpression::NameContext::Tag& name_ctx);
	void MarkCapturedReadByName(const Token& name);

	std::vector<FunctionContext> m_functions;
};
