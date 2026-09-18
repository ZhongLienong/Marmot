#include "TailCallOptimization.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/Analysis/SharedAnalysis.h"

namespace
{
	bool ContainsRecursiveCallImpl(const MidoriExpression& expr, std::string_view function_name);

	bool ContainsRecursiveCallInStatement(const MidoriStatement& stmt, std::string_view function_name)
	{
		if (stmt.IsStatement<MidoriStatement::ExpressionStatement>())
		{
			return ContainsRecursiveCallImpl(*stmt.GetStatement<MidoriStatement::ExpressionStatement>().m_expr, function_name);
		}
		if (stmt.IsStatement<MidoriStatement::VariableDefinition>())
		{
			return ContainsRecursiveCallImpl(*stmt.GetStatement<MidoriStatement::VariableDefinition>().m_value, function_name);
		}
		if (stmt.IsStatement<MidoriStatement::TupleDefinition>())
		{
			return ContainsRecursiveCallImpl(*stmt.GetStatement<MidoriStatement::TupleDefinition>().m_value, function_name);
		}
		return false;
	}

	bool ContainsRecursiveCallInBlockStatements(const MidoriExpression::Block& block, std::string_view function_name)
	{
		for (const std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
		{
			if (ContainsRecursiveCallInStatement(*stmt, function_name))
			{
				return true;
			}
		}
		return false;
	}

	bool ContainsRecursiveCallImpl(const MidoriExpression& expr, std::string_view function_name)
	{
		struct RecursiveCallVisitor
		{
			std::string_view m_function_name;

			bool operator()(const MidoriExpression::Call& node) const
			{
				const MidoriExpression* callee_expr = MidoriAnalysis::StripRedundantGroups(node.m_callee.get());
				if (callee_expr && callee_expr->IsExpression<MidoriExpression::NameAccess>())
				{
					const MidoriExpression::NameAccess& callee_name = callee_expr->GetExpression<MidoriExpression::NameAccess>();
					if (callee_name.m_name.m_lexeme == m_function_name)
					{
						return true;
					}
				}

				if (ContainsRecursiveCallImpl(*node.m_callee, m_function_name))
				{
					return true;
				}

				for (const std::unique_ptr<MidoriExpression>& arg : node.m_arguments)
				{
					if (ContainsRecursiveCallImpl(*arg, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::As& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_expr, m_function_name);
			}

			bool operator()(const MidoriExpression::Binary& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_left, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_right, m_function_name);
			}

			bool operator()(const MidoriExpression::Group& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_expr_in, m_function_name);
			}

			bool operator()(const MidoriExpression::Tuple& node) const
			{
				for (const std::unique_ptr<MidoriExpression>& elem : node.m_elements)
				{
					if (ContainsRecursiveCallImpl(*elem, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::UnaryPrefix& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_expr, m_function_name);
			}

			bool operator()(const MidoriExpression::UnarySuffix& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_expr, m_function_name);
			}

			bool operator()(const MidoriExpression::Spawn& node) const
			{
				for (const std::unique_ptr<MidoriExpression>& argument : node.m_arguments)
				{
					if (ContainsRecursiveCallImpl(*argument, m_function_name))
					{
						return true;
					}
				}
				return ContainsRecursiveCallImpl(*node.m_callee, m_function_name);
			}

			bool operator()(const MidoriExpression::Join& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_worker, m_function_name);
			}

			bool operator()(const MidoriExpression::ChannelCreate& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_capacity, m_function_name);
			}

			bool operator()(const MidoriExpression::Send& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_channel, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_value, m_function_name);
			}

			bool operator()(const MidoriExpression::Receive& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_channel, m_function_name);
			}

			bool operator()(const MidoriExpression::NameAccess&) const
			{
				return false;
			}

			bool operator()(const MidoriExpression::Function&) const
			{
				return false;
			}

			bool operator()(const MidoriExpression::Construct& node) const
			{
				for (const std::unique_ptr<MidoriExpression>& param : node.m_params)
				{
					if (ContainsRecursiveCallImpl(*param, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::RecordUpdate& node) const
			{
				if (ContainsRecursiveCallImpl(*node.m_source, m_function_name))
				{
					return true;
				}

				for (const MidoriExpression::RecordUpdate::FieldUpdate& update : node.m_updates)
				{
					if (ContainsRecursiveCallImpl(*update.m_value, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::IfElse& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_condition, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_true_branch, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_else_branch, m_function_name);
			}

			bool operator()(const MidoriExpression::MemberAccess& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_struct, m_function_name);
			}

			bool operator()(const MidoriExpression::Array& node) const
			{
				for (const std::unique_ptr<MidoriExpression>& elem : node.m_elems)
				{
					if (ContainsRecursiveCallImpl(*elem, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::IndexAccess& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_arr_var, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_index, m_function_name);
			}

			bool operator()(const MidoriExpression::ArrayComprehension& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_transform_expr, m_function_name) || ContainsRecursiveCallImpl(*node.m_range, m_function_name);
			}

			bool operator()(const MidoriExpression::RangeBinary& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_start, m_function_name) || ContainsRecursiveCallImpl(*node.m_end, m_function_name);
			}

			bool operator()(const MidoriExpression::RangeTernary& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_start, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_step, m_function_name)
					|| ContainsRecursiveCallImpl(*node.m_end, m_function_name);
			}

			bool operator()(const MidoriExpression::Block& node) const
			{
				if (ContainsRecursiveCallInBlockStatements(node, m_function_name))
				{
					return true;
				}
				return node.m_final_expr.has_value() && ContainsRecursiveCallImpl(*node.m_final_expr.value(), m_function_name);
			}

			bool operator()(const MidoriExpression::Match& node) const
			{
				if (ContainsRecursiveCallImpl(*node.m_arg_expr, m_function_name))
				{
					return true;
				}
				for (const std::unique_ptr<MidoriExpression>& case_expr : node.m_cases)
				{
					if (ContainsRecursiveCallImpl(*case_expr, m_function_name))
					{
						return true;
					}
				}
				return false;
			}

			bool operator()(const MidoriExpression::Case& node) const
			{
				if (node.HasGuard() && ContainsRecursiveCallImpl(*node.m_guard.value(), m_function_name))
				{
					return true;
				}

				return ContainsRecursiveCallImpl(*node.m_expr, m_function_name);
			}

			bool operator()(const MidoriExpression::For& node) const
			{
				return ContainsRecursiveCallImpl(*node.m_range, m_function_name) || ContainsRecursiveCallImpl(*node.m_body, m_function_name);
			}

			bool operator()(const MidoriExpression::Literal&) const
			{
				return false;
			}

		};

		return std::visit(RecursiveCallVisitor{ function_name }, *expr);
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

void TailCallOptimization::MarkTailRecursion(const std::string& function_name, std::unique_ptr<MidoriExpression>& body)
{
	m_current_function = function_name;
	m_has_tail_recursion = false;
	m_marked_new_tail_call = false;

	m_has_tail_recursion = IsTailRecursive(body, m_current_function);
	if (m_marked_new_tail_call)
	{
		MarkOptimization();
	}

	m_current_function.clear();

	VisitAndReplace(body);
}

void TailCallOptimization::operator()(MidoriStatement::FunctionDefinition& defun)
{
	MarkTailRecursion(defun.m_name.m_lexeme, defun.m_body);
}

void TailCallOptimization::operator()(MidoriStatement::VariableDefinition& def)
{
	// `def Name = fn(...)` names its own procedure, so a call to Name in tail position
	// inside the lambda is self-recursion.
	if (def.m_value != nullptr && def.m_value->IsExpression<MidoriExpression::Function>())
	{
		MarkTailRecursion(def.m_name.m_lexeme, def.m_value->GetExpression<MidoriExpression::Function>().m_body);
		return;
	}

	VisitAndReplace(def.m_value);
}

void TailCallOptimization::operator()(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
	{
		if (stmt->IsStatement<MidoriStatement::FunctionDefinition>())
		{
			MidoriStatement::FunctionDefinition& nested_defun = stmt->GetStatement<MidoriStatement::FunctionDefinition>();
			(*this)(nested_defun);
		}
		else if (stmt->IsStatement<MidoriStatement::VariableDefinition>())
		{
			MidoriStatement::VariableDefinition& nested_def = stmt->GetStatement<MidoriStatement::VariableDefinition>();
			if (nested_def.m_value != nullptr && nested_def.m_value->IsExpression<MidoriExpression::Function>())
			{
				(*this)(nested_def);
			}
		}
	}

	if (block.m_final_expr.has_value())
	{
		VisitAndReplace(block.m_final_expr.value());
	}
}

bool TailCallOptimization::IsTailCall(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name)
{
	if (!expr->IsExpression<MidoriExpression::Call>())
	{
		return false;
	}

	MidoriExpression::Call& call = expr->GetExpression<MidoriExpression::Call>();
	const MidoriExpression* callee_expr = MidoriAnalysis::StripRedundantGroups(call.m_callee.get());
	if (callee_expr && callee_expr->IsExpression<MidoriExpression::NameAccess>())
	{
		const MidoriExpression::NameAccess& callee_name = callee_expr->GetExpression<MidoriExpression::NameAccess>();
		if (callee_name.m_name.m_lexeme == function_name)
		{
			if (!call.m_is_tail_call)
			{
				call.m_is_tail_call = true;
				m_marked_new_tail_call = true;
			}
			return true;
		}
	}
	return false;
}

bool TailCallOptimization::ContainsRecursiveCall(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name)
{
	return ContainsRecursiveCallImpl(*expr, function_name);
}

bool TailCallOptimization::IsTailRecursive(std::unique_ptr<MidoriExpression>& expr, std::string_view function_name)
{
	if (IsTailCall(expr, function_name))
	{
		return true;
	}

	if (expr->IsExpression<MidoriExpression::IfElse>())
	{
		MidoriExpression::IfElse& if_else = expr->GetExpression<MidoriExpression::IfElse>();

		if (ContainsRecursiveCallImpl(*if_else.m_condition, function_name))
		{
			return false;
		}

		bool then_has_call = ContainsRecursiveCallImpl(*if_else.m_true_branch, function_name);
		bool else_has_call = ContainsRecursiveCallImpl(*if_else.m_else_branch, function_name);
		bool then_is_tail = IsTailRecursive(if_else.m_true_branch, function_name);
		bool else_is_tail = IsTailRecursive(if_else.m_else_branch, function_name);

		// If a branch has a call but it's NOT in tail position, reject the whole function
		if ((then_has_call && !then_is_tail) || (else_has_call && !else_is_tail))
		{
			return false;
		}

		// At least one branch must have a tail call
		return then_is_tail || else_is_tail;
	}

	if (expr->IsExpression<MidoriExpression::Block>())
	{
		MidoriExpression::Block& block = expr->GetExpression<MidoriExpression::Block>();

		// Only the final expression is in tail position: a recursive call in any
		// statement before it is followed by more work.
		for (std::unique_ptr<MidoriStatement>& stmt : block.m_stmts)
		{
			if (stmt->IsStatement<MidoriStatement::ExpressionStatement>())
			{
				if (ContainsRecursiveCallImpl(*stmt->GetStatement<MidoriStatement::ExpressionStatement>().m_expr, function_name))
				{
					return false;
				}
			}
			else if (stmt->IsStatement<MidoriStatement::VariableDefinition>())
			{
				if (ContainsRecursiveCallImpl(*stmt->GetStatement<MidoriStatement::VariableDefinition>().m_value, function_name))
				{
					return false;
				}
			}
			else if (stmt->IsStatement<MidoriStatement::TupleDefinition>())
			{
				if (ContainsRecursiveCallImpl(*stmt->GetStatement<MidoriStatement::TupleDefinition>().m_value, function_name))
				{
					return false;
				}
			}
		}

		if (block.m_final_expr.has_value())
		{
			bool final_has_call = ContainsRecursiveCallImpl(*block.m_final_expr.value(), function_name);
			bool final_is_tail = IsTailRecursive(block.m_final_expr.value(), function_name);
			if (final_has_call && !final_is_tail)
			{
				return false;
			}
			return final_is_tail;
		}

		return false;
	}
	if (expr->IsExpression<MidoriExpression::Case>())
	{
		MidoriExpression::Case& case_expr = expr->GetExpression<MidoriExpression::Case>();
		if (case_expr.HasGuard() && ContainsRecursiveCallImpl(*case_expr.m_guard.value(), function_name))
		{
			return false;
		}

		return IsTailRecursive(case_expr.m_expr, function_name);
	}
	if (expr->IsExpression<MidoriExpression::Group>())
	{
		MidoriExpression::Group& group = expr->GetExpression<MidoriExpression::Group>();
		return IsTailRecursive(group.m_expr_in, function_name);
	}
	if (expr->IsExpression<MidoriExpression::Match>())
	{
		MidoriExpression::Match& match_expr = expr->GetExpression<MidoriExpression::Match>();

		if (ContainsRecursiveCallImpl(*match_expr.m_arg_expr, function_name))
		{
			return false;
		}

		bool has_tail_call = false;
		for (std::unique_ptr<MidoriExpression>& case_expr : match_expr.m_cases)
		{
			bool case_has_call = ContainsRecursiveCallImpl(*case_expr, function_name);
			bool case_is_tail = IsTailRecursive(case_expr, function_name);

			// If this case has a call but it's NOT in tail position, reject
			if (case_has_call && !case_is_tail)
			{
				return false;
			}

			if (case_is_tail)
			{
				has_tail_call = true;
			}
		}

		return has_tail_call;
	}

	// For any other expression type, if it contains a recursive call, it's NOT in tail position
	if (ContainsRecursiveCall(expr, function_name))
	{
		return false;
	}

	return false;
}
