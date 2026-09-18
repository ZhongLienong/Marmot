#include "ConstantBranchElimination.h"

#include "Compiler/Analysis/SharedAnalysis.h"
#include "Compiler/Token/Token.h"

MidoriResult::OptimizerResult ConstantBranchElimination::Optimize(MidoriProgramTree program_tree)
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

std::string_view ConstantBranchElimination::GetName() const
{
	return "ConstantBranchElimination";
}

void ConstantBranchElimination::operator()(MidoriExpression::IfElse& if_else)
{
	VisitAndReplace(if_else.m_condition);
	VisitAndReplace(if_else.m_true_branch);
	VisitAndReplace(if_else.m_else_branch);

	std::optional<bool> condition_value = MidoriAnalysis::TryEvalTruthValue(*if_else.m_condition);
	if (condition_value.has_value() && MidoriAnalysis::IsPure(*if_else.m_condition))
	{
		m_pending_replacement = condition_value.value()
			? MidoriAnalysis::StripRedundantGroups(std::move(if_else.m_true_branch))
			: MidoriAnalysis::StripRedundantGroups(std::move(if_else.m_else_branch));
		m_pending_replacement->GetType() = if_else.m_type_data;
		return;
	}

	if (IsBoolLiteral(*if_else.m_true_branch, true) && IsBoolLiteral(*if_else.m_else_branch, false))
	{
		m_pending_replacement = MidoriAnalysis::StripRedundantGroups(std::move(if_else.m_condition));
		m_pending_replacement->GetType() = if_else.m_type_data;
		return;
	}

	if (IsBoolLiteral(*if_else.m_true_branch, false) && IsBoolLiteral(*if_else.m_else_branch, true))
	{
		m_pending_replacement = MakeLogicalNot(if_else);
		m_pending_replacement->GetType() = if_else.m_type_data;
	}
}

void ConstantBranchElimination::operator()(MidoriExpression::Match& match)
{
	VisitAndReplace(match.m_arg_expr);

	for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
	{
		VisitAndReplace(case_expr);
	}

	if (!MidoriAnalysis::IsPure(*match.m_arg_expr))
	{
		return;
	}

	for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
	{
		if (!case_expr->IsExpression<MidoriExpression::Case>())
		{
			return;
		}

		MidoriExpression::Case& match_case = case_expr->GetExpression<MidoriExpression::Case>();
		if (match_case.HasGuard())
		{
			return;
		}

		const std::optional<bool> pattern_matches = MidoriAnalysis::TryMatchPattern(*match_case.m_pattern, *match.m_arg_expr);
		if (!pattern_matches.has_value())
		{
			return;
		}

		if (!pattern_matches.value())
		{
			continue;
		}

		if (match_case.m_binding_count != 0 || !MidoriAnalysis::IsBindingFreePattern(*match_case.m_pattern))
		{
			return;
		}

		m_pending_replacement = MidoriAnalysis::StripRedundantGroups(std::move(match_case.m_expr));
		m_pending_replacement->GetType() = match.m_type_data;
		return;
	}
}

std::unique_ptr<MidoriExpression> ConstantBranchElimination::MakeLogicalNot(MidoriExpression::IfElse& if_else)
{
	std::unique_ptr<MidoriExpression> condition = MidoriAnalysis::StripRedundantGroups(std::move(if_else.m_condition));
	Token bang_token("!", Token::Name::BANG, if_else.m_if_token.m_line, if_else.m_if_token.m_file_name);
	return std::make_unique<MidoriExpression>(MidoriExpression::UnaryPrefix(bang_token, std::move(condition)));
}

bool ConstantBranchElimination::IsBoolLiteral(const MidoriExpression& expr, bool expected_value)
{
	const MidoriExpression* stripped_expr = MidoriAnalysis::StripRedundantGroups(&expr);
	if (stripped_expr == nullptr || !stripped_expr->IsLiteral(MidoriExpression::LiteralKind::Bool))
	{
		return false;
	}

	const MidoriExpression::Literal& bool_expr = stripped_expr->GetExpression<MidoriExpression::Literal>();
	return (bool_expr.m_token.m_token_name == Token::Name::TRUE) == expected_value;
}
