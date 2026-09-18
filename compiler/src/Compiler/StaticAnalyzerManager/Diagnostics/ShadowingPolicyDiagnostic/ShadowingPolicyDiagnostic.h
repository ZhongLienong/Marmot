#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ShadowingPolicyDiagnostic final : public AstDiagnosticPass
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
	void operator()(MidoriStatement::Struct& struct_stmt) override;
	void operator()(MidoriStatement::Union& union_stmt) override;
	void operator()(MidoriStatement::TypeAlias& type_alias) override;
	void operator()(MidoriPattern::Binding& binding) override;
	void operator()(MidoriExpression::Function& function) override;
	void operator()(MidoriExpression::Block& block) override;
	void operator()(MidoriExpression::Case& case_expr) override;

private:
	struct Scope
	{
		std::unordered_set<std::string> m_variables;
		std::unordered_set<std::string> m_structs;
		std::unordered_set<std::string> m_union_constructors;
		std::unordered_set<std::string> m_types;
	};

	void PushScope();
	void PopScope();
	void DefineVariable(const Token& token);
	void DefineStruct(const Token& token);
	void DefineUnionConstructor(const Token& token);
	void DefineType(const Token& token);
	void DefineGenericParams(const std::vector<Token>& generic_params);
	std::vector<std::string_view> CollectShadowKinds(const std::string& mangled_name) const;
	void EmitShadowWarning(const Token& token, const std::vector<std::string_view>& kinds);

	std::vector<Scope> m_scopes;
};
