#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class ConstantFolding : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

protected:
	using MidoriOptimizer::operator();

	void operator()(MidoriExpression::Binary& binary) override;

	void operator()(MidoriExpression::UnaryPrefix& unary) override;
};
