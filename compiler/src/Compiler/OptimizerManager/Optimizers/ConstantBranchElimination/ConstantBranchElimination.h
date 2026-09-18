#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class ConstantBranchElimination final : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

private:
	void operator()(MidoriExpression::IfElse& if_else) override;

	void operator()(MidoriExpression::Match& match) override;

	std::unique_ptr<MidoriExpression> MakeLogicalNot(MidoriExpression::IfElse& if_else);

	static bool IsBoolLiteral(const MidoriExpression& expr, bool expected_value);
};
