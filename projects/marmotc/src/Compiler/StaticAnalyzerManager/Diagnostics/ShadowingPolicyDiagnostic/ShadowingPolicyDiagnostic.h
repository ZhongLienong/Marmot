#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ShadowingPolicyDiagnostic final : public DiagnosticPass
{
protected:
	void Reset(MarmotProgramTree& program_tree) override;

	void Finish() override;

	void operator()(MarmotStatement::VariableDefinition& def) override;
	void operator()(MarmotStatement::TupleDefinition& def_tuple) override;
	void operator()(MarmotStatement::FunctionDefinition& defun) override;
	void operator()(MarmotStatement::ForeignDefinition& foreign) override;
	void operator()(MarmotStatement::Struct& struct_stmt) override;
	void operator()(MarmotStatement::Union& union_stmt) override;
	void operator()(MarmotStatement::TypeAlias& type_alias) override;
	void operator()(MarmotPattern::Binding& binding) override;
	void operator()(MarmotExpression::Function& function) override;
	void operator()(MarmotExpression::Block& block) override;
	void operator()(MarmotExpression::Case& case_expr) override;

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
