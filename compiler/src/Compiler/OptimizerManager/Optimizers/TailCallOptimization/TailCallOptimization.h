#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

// Marks every call in a function body's tail position, whatever it calls: with
// no loops in the language, mutual recursion and one function handing off to
// another are how a program iterates, and each would otherwise keep a frame.
class TailCallOptimization : public MidoriOptimizer
{
public:

	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override;

protected:
	using MidoriOptimizer::operator();

	void operator()(MidoriStatement::FunctionDefinition& defun) override;

	void operator()(MidoriExpression::Function& function) override;

private:

	void MarkTailCalls(MidoriExpression& expr);
};
