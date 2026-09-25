#include "TailCallOptimization.h"

namespace
{
	int MarkTailCallsIn(MidoriExpression& expr);

	struct TailPositionVisitor
	{
		int operator()(MidoriExpression::Call& call) const
		{
			if (call.m_is_foreign || call.m_is_tail_call)
			{
				return 0;
			}

			call.m_is_tail_call = true;
			return 1;
		}

		int operator()(MidoriExpression::IfElse& if_else) const
		{
			return MarkTailCallsIn(*if_else.m_true_branch) + MarkTailCallsIn(*if_else.m_else_branch);
		}

		int operator()(MidoriExpression::Block& block) const
		{
			return block.m_final_expr.has_value() ? MarkTailCallsIn(*block.m_final_expr.value()) : 0;
		}

		int operator()(MidoriExpression::Match& match) const
		{
			int marked = 0;
			for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
			{
				marked += MarkTailCallsIn(*case_expr);
			}
			return marked;
		}

		int operator()(MidoriExpression::Case& case_expr) const
		{
			return MarkTailCallsIn(*case_expr.m_expr);
		}

		int operator()(MidoriExpression::Group& group) const
		{
			return MarkTailCallsIn(*group.m_expr_in);
		}

		template<typename Expression>
		int operator()(Expression&) const
		{
			return 0;
		}
	};

	int MarkTailCallsIn(MidoriExpression& expr)
	{
		return std::visit(TailPositionVisitor{}, *expr);
	}
}

MidoriResult::OptimizerResult TailCallOptimization::Optimize(MidoriProgramTree program_tree)
{
	ResetPassState();
	std::ranges::for_each
	(
		program_tree,
		[this](std::unique_ptr<MidoriStatement>& stmt)
		{
			VisitStatement(stmt);
		}
	);
	return std::move(program_tree);
}

std::string_view TailCallOptimization::GetName() const
{
	return "TailCallOptimization";
}

void TailCallOptimization::operator()(MidoriStatement::FunctionDefinition& defun)
{
	MarkTailCalls(*defun.m_body);
	MidoriOptimizer::operator()(defun);
}

void TailCallOptimization::operator()(MidoriExpression::Function& function)
{
	MarkTailCalls(*function.m_body);
	MidoriOptimizer::operator()(function);
}

void TailCallOptimization::MarkTailCalls(MidoriExpression& expr)
{
	if (MarkTailCallsIn(expr) > 0)
	{
		MarkOptimization();
	}
}
