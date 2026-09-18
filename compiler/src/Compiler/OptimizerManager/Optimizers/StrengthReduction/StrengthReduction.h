#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class StrengthReduction : public MidoriOptimizer
{
public:

	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

protected:
	using MidoriOptimizer::operator();

	void operator()(MidoriExpression::Binary& binary) override;

private:
	static double GetFloatValue(MidoriExpression::Literal* float_lit);

	static int64_t IsPowerOfTwo(MidoriInteger value);

	std::unique_ptr<MidoriExpression> TryReduceBinary(MidoriExpression::Binary& binary, const Token& op, MidoriExpression::Literal* left_int, MidoriExpression::Literal* right_int, MidoriExpression::Literal* left_float, MidoriExpression::Literal* right_float);
};
