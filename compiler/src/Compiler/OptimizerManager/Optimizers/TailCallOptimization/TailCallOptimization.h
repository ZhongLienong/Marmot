#pragma once

#include "Compiler/OptimizerManager/Optimizers/BaseOptimizer/BaseOptimizer.h"

class TailCallOptimization : public MidoriOptimizer
{
private:
	std::string m_current_function;
	bool m_has_tail_recursion = false;
	bool m_marked_new_tail_call = false;

public:

	MidoriResult::OptimizerResult Optimize(MidoriProgramTree program_tree) override;

	std::string_view GetName() const override; 

protected:
	using MidoriOptimizer::operator();

	void operator()(MidoriStatement::FunctionDefinition& defun) override;

	void operator()(MidoriStatement::VariableDefinition& def) override;

	void operator()(MidoriExpression::Block& block) override;

private:

	void MarkTailRecursion(const std::string& function_name, std::unique_ptr<MidoriExpression>& body);

	bool IsTailCall(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name);

	bool IsTailRecursive(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name);

	bool ContainsRecursiveCall(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name);
};
