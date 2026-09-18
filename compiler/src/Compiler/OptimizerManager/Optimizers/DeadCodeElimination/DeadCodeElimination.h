#pragma once

#include "Compiler/Analysis/SharedAnalysis.h"
#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class DeadCodeElimination final : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

private:
	void ProcessTopLevelStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements);

	void RemovePureExpressionStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements);

	void ElideUnusedPureLocalDefinitions(MidoriExpression::Block& block);

	static bool HasNestedCallableBoundaryAfter(const MidoriAnalysis::BlockLocalAccessSummary& access_summary, std::size_t statement_index);

	void operator()(MidoriStatement::FunctionDefinition& defun) override;

	void operator()(MidoriExpression::Block& block) override;
};
