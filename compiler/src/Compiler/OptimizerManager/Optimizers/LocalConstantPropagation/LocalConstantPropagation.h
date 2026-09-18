#pragma once

#include "Compiler/Analysis/SharedAnalysis.h"
#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

class LocalConstantPropagation final : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

private:
	struct AliasReplacement
	{
		Token m_name;
		int m_local_index = -1;
	};

	struct Replacement
	{
		std::variant<MidoriAnalysis::ConstantValue, AliasReplacement> m_value;
	};

	using Environment = std::unordered_map<int, Replacement>;

	std::vector<Environment> m_environments;

	Environment& CurrentEnvironment();

	const Environment& CurrentEnvironment() const;

	void PushEnvironment(Environment&& environment);

	Environment PopEnvironment();

	void ProcessStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements);

	void FinalizeStatement(MidoriStatement& statement);

	void Finalize(const MidoriStatement::ExpressionStatement& stmt);

	void Finalize(const MidoriStatement::VariableDefinition& def);

	void Finalize(const MidoriStatement::TupleDefinition& def_tuple);

	void Finalize(const MidoriStatement::FunctionDefinition& defun);


	void Finalize(const MidoriStatement::ForeignDefinition& foreign);

	void Finalize(const MidoriStatement::Struct& struct_stmt);

	void Finalize(const MidoriStatement::Union& union_stmt);

	void Finalize(const MidoriStatement::Class& typeclass_stmt);

	void Finalize(const MidoriStatement::Instance& instance_stmt);

	void Finalize(const MidoriStatement::TypeAlias& type_alias);

	Environment VisitInEnvironment(std::unique_ptr<MidoriExpression>& expr, const Environment& environment);

	void VisitInFreshEnvironment(std::unique_ptr<MidoriExpression>& expr);

	void InvalidateLocal(int local_index);

	void ClearAllReplacements();

	void ClearAliasReplacements();

	Environment FilterRepeatedEnvironment(const Environment& environment, const MidoriExpression& expr) const;

	std::optional<Replacement> TryCreateReplacement(const MidoriExpression& expr) const;

	std::unique_ptr<MidoriExpression> MaterializeReplacement(const Replacement& replacement, const MidoriExpression::NameAccess& use_site) const;

	static bool ReplacementDependsOnLocal(const Replacement& replacement, int local_index);

	static bool AreEquivalent(const Replacement& left, const Replacement& right);

	static Environment IntersectEnvironments(const Environment& baseline, const Environment& left, const Environment& right);

	void MergeParentEnvironment(const Environment& incoming, const Environment& completed);

	void operator()(MidoriStatement::FunctionDefinition& defun) override;

	void operator()(MidoriExpression::Binary& binary) override;

	void operator()(MidoriExpression::NameAccess& variable) override;



	void operator()(MidoriExpression::Call& call) override;

	void operator()(MidoriExpression::Function& function) override;

	void operator()(MidoriExpression::ArrayComprehension& comp) override;



	void operator()(MidoriExpression::IfElse& if_else) override;

	void operator()(MidoriExpression::Block& block) override;

	void operator()(MidoriExpression::Match& match) override;


	void operator()(MidoriExpression::For& for_expr) override;
};
