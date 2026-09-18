#include "DeadCodeElimination.h"

#include "Compiler/Analysis/SharedAnalysis.h"

#include <cstddef>
#include <type_traits>

MidoriResult::OptimizerResult DeadCodeElimination::Optimize(MidoriProgramTree program_tree)
{
	ResetPassState();
	ProcessTopLevelStatements(program_tree);
	return std::move(program_tree);
}

std::string_view DeadCodeElimination::GetName() const
{
	return "DeadCodeElimination";
}

void DeadCodeElimination::ProcessTopLevelStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements)
{
	for (std::unique_ptr<MidoriStatement>& statement : statements)
	{
		VisitStatement(statement);
	}

	RemovePureExpressionStatements(statements);
}

void DeadCodeElimination::RemovePureExpressionStatements(std::vector<std::unique_ptr<MidoriStatement>>& statements)
{
	for (std::vector<std::unique_ptr<MidoriStatement>>::iterator it = statements.begin(); it != statements.end();)
	{
		MidoriStatement* statement = it->get();
		if (!statement->IsStatement<MidoriStatement::ExpressionStatement>())
		{
			++it;
			continue;
		}

		const MidoriExpression& expression = *statement->GetStatement<MidoriStatement::ExpressionStatement>().m_expr;
		if (!MidoriAnalysis::IsPure(expression))
		{
			++it;
			continue;
		}

		it = statements.erase(it);
		MarkOptimization();
	}
}

void DeadCodeElimination::ElideUnusedPureLocalDefinitions(MidoriExpression::Block& block)
{
	const MidoriAnalysis::BlockLocalAccessSummary access_summary = MidoriAnalysis::AnalyzeBlockLocalAccess(block);

	for (std::size_t index = 0u; index < block.m_stmts.size(); index += 1u)
	{
		MidoriStatement* statement = block.m_stmts[index].get();
		if (!statement->IsStatement<MidoriStatement::VariableDefinition>())
		{
			continue;
		}

		MidoriStatement::VariableDefinition& definition = statement->GetStatement<MidoriStatement::VariableDefinition>();
		if (definition.m_is_elided || !definition.m_local_index.has_value())
		{
			continue;
		}

		if (!MidoriAnalysis::IsPure(*definition.m_value))
		{
			continue;
		}

		const int local_index = definition.m_local_index.value();
		if (access_summary.IsLocalUsedAfter(local_index, index)
			|| access_summary.IsLocalAssignedAfter(local_index, index)
			|| HasNestedCallableBoundaryAfter(access_summary, index))
		{
			continue;
		}

		definition.m_is_elided = true;
		MarkOptimization();
	}
}

bool DeadCodeElimination::HasNestedCallableBoundaryAfter(const MidoriAnalysis::BlockLocalAccessSummary& access_summary, std::size_t statement_index)
{
	const std::size_t start_index = statement_index + 1u;
	for (std::size_t index = start_index; index < access_summary.m_statement_summaries.size(); index += 1u)
	{
		if (access_summary.m_statement_summaries[index].m_has_nested_callable_boundary)
		{
			return true;
		}
	}

	return access_summary.m_final_expression_summary.has_value()
		&& access_summary.m_final_expression_summary->m_has_nested_callable_boundary;
}

void DeadCodeElimination::operator()(MidoriStatement::FunctionDefinition& defun)
{
	VisitAndReplace(defun.m_body);
}

void DeadCodeElimination::operator()(MidoriExpression::Block& block)
{
	for (std::unique_ptr<MidoriStatement>& statement : block.m_stmts)
	{
		VisitStatement(statement);
	}

	if (block.m_final_expr.has_value())
	{
		VisitAndReplace(block.m_final_expr.value());
	}

	RemovePureExpressionStatements(block.m_stmts);
	ElideUnusedPureLocalDefinitions(block);
}
