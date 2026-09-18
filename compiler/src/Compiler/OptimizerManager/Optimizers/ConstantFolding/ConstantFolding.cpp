#include "ConstantFolding.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/Analysis/SharedAnalysis.h"

MidoriResult::OptimizerResult ConstantFolding::Optimize(MidoriProgramTree program_tree)
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

std::string_view ConstantFolding::GetName() const
{
	return "ConstantFolding";
}

void ConstantFolding::operator()(MidoriExpression::Binary& binary)
{
	VisitAndReplace(binary.m_left);
	VisitAndReplace(binary.m_right);

	std::optional<MidoriAnalysis::ConstantValue> folded_value = MidoriAnalysis::TryEvalConstant(binary);
	if (!folded_value.has_value())
	{
		return;
	}

	m_pending_replacement = MidoriAnalysis::MakeLiteralExpression(folded_value.value(), binary.m_op);
	m_pending_replacement->GetType() = binary.m_type_data;
}

void ConstantFolding::operator()(MidoriExpression::UnaryPrefix& unary)
{
	VisitAndReplace(unary.m_expr);

	std::optional<MidoriAnalysis::ConstantValue> folded_value = MidoriAnalysis::TryEvalConstant(unary);
	if (!folded_value.has_value())
	{
		return;
	}

	m_pending_replacement = MidoriAnalysis::MakeLiteralExpression(folded_value.value(), unary.m_op);
	m_pending_replacement->GetType() = unary.m_type_data;
}
