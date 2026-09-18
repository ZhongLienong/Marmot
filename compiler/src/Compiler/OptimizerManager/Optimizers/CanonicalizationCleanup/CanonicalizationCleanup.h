#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class CanonicalizationCleanup final : public MidoriOptimizer
{
public:
	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

	using MidoriOptimizer::operator();

	void operator()(MidoriExpression::As& as) override;

	void operator()(MidoriExpression::Binary& binary) override;

	void operator()(MidoriExpression::Group& group) override;

	void operator()(MidoriExpression::MemberAccess& get) override;

	void operator()(MidoriExpression::IndexAccess& array_get) override;

	void operator()(MidoriExpression::UnaryPrefix& unary) override;

	void operator()(MidoriExpression::ArrayComprehension& comp) override;

	void operator()(MidoriExpression::For& for_expr) override;
};
