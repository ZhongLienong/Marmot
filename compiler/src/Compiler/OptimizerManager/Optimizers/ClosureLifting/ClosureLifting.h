#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

#include <unordered_set>

class ClosureLifting : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

protected:
	using MidoriOptimizer::operator();

	void operator()(MidoriStatement::VariableDefinition& def) override;

	void operator()(MidoriExpression::Function& function) override;

	void operator()(MidoriExpression::Block& block) override;

private:
	void RecordVisibleGlobalNames(const MidoriStatement& stmt);

	std::unordered_set<std::string> m_visible_globals;
	std::vector<std::unique_ptr<MidoriStatement>> m_pending_globals;
};
