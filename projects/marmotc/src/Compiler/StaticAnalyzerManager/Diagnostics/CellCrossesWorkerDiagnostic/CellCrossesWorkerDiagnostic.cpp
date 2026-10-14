#include "CellCrossesWorkerDiagnostic.h"

#include <format>
#include <utility>
#include <variant>
#include <vector>

namespace
{
	constexpr std::string_view CellCrossesWorkerMessage = "This reference is copied into the worker; writes on either side are not seen by the other.";
	constexpr std::string_view CellComesBackMessage = "This reference is copied out of the worker; writes on either side are not seen by the other.";

	struct CarriedCells
	{
		// Each a local from outside the function, at its first use inside it.
		std::vector<Token> m_captured;
		// Each a global, at its first read.
		std::vector<Token> m_globals;
	};

	// Finds the cells a function value takes into a worker. The parser numbers every
	// enclosing local in one sequence, and a function's captured count is how many
	// were numbered before its own, so a Cell name anywhere inside it, in a lambda
	// within it too, whose index is below that count is a local from outside it.
	// The globals it reads go too, and those read by this module's functions it
	// names, followed in turn. A global reached only through another global's value,
	// or through another module's function, is not seen.
	class CarriedCellFinder final : public MidoriAbstractSyntaxTreeWalker
	{
	public:
		explicit CarriedCellFinder(const std::unordered_map<std::string, MidoriExpression::Function*>& module_functions)
			: m_module_functions(module_functions)
		{
		}

		CarriedCells Find(MidoriExpression::Function& function)
		{
			m_visited_functions.insert(&function);
			m_captured_count = function.m_captured_count;
			VisitExpression(function.m_body);

			// A module function captures nothing.
			m_captured_count = 0;
			while (!m_pending_functions.empty())
			{
				MidoriExpression::Function* named = m_pending_functions.back();
				m_pending_functions.pop_back();
				VisitExpression(named->m_body);
			}
			return std::move(m_found);
		}

	protected:
		void operator()(MidoriExpression::NameAccess& access) override
		{
			if (const MidoriExpression::NameContext::Cell* captured = std::get_if<MidoriExpression::NameContext::Cell>(&access.m_name_ctx))
			{
				if (captured->m_index < m_captured_count && ContainsCell(access.m_type_data) && m_seen_cells.insert(captured->m_index).second)
				{
					m_found.m_captured.emplace_back(access.m_name);
				}
				return;
			}

			if (!std::holds_alternative<MidoriExpression::NameContext::Global>(access.m_name_ctx))
			{
				return;
			}

			if (ContainsCell(access.m_type_data))
			{
				if (m_seen_globals.insert(access.m_name.m_lexeme).second)
				{
					m_found.m_globals.emplace_back(access.m_name);
				}
				return;
			}

			const std::unordered_map<std::string, MidoriExpression::Function*>::const_iterator named = m_module_functions.find(access.m_name.m_lexeme);
			if (named != m_module_functions.cend() && m_visited_functions.insert(named->second).second)
			{
				m_pending_functions.push_back(named->second);
			}
		}

	private:
		const std::unordered_map<std::string, MidoriExpression::Function*>& m_module_functions;
		int m_captured_count = 0;
		std::vector<MidoriExpression::Function*> m_pending_functions;
		std::unordered_set<const MidoriExpression::Function*> m_visited_functions;
		std::unordered_set<int> m_seen_cells;
		std::unordered_set<std::string> m_seen_globals;
		CarriedCells m_found;
	};
}

CellCrossesWorkerDiagnostic::CellCrossesWorkerDiagnostic(const WorkerCrossings& crossings)
	: m_crossings(crossings)
{
}

void CellCrossesWorkerDiagnostic::Reset(MidoriProgramTree& program_tree)
{
	m_module_functions.clear();
	for (std::unique_ptr<MidoriStatement>& statement : program_tree)
	{
		if (!statement->IsStatement<MidoriStatement::VariableDefinition>())
		{
			continue;
		}

		MidoriStatement::VariableDefinition& definition = statement->GetStatement<MidoriStatement::VariableDefinition>();
		if (definition.m_value->IsExpression<MidoriExpression::Function>())
		{
			m_module_functions.emplace(definition.m_name.m_lexeme, &definition.m_value->GetExpression<MidoriExpression::Function>());
		}
	}
}

void CellCrossesWorkerDiagnostic::WarnIfCell(const MidoriExpression& expression, const Token& token)
{
	if (ContainsCell(expression.GetType()))
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, token, CellCrossesWorkerMessage);
	}
}

void CellCrossesWorkerDiagnostic::WarnIfFunctionCarriesCells(MidoriExpression& value, const Token& token)
{
	MidoriExpression::Function* function = nullptr;
	if (value.IsExpression<MidoriExpression::Function>())
	{
		function = &value.GetExpression<MidoriExpression::Function>();
	}
	else if (value.IsExpression<MidoriExpression::NameAccess>() && std::holds_alternative<MidoriExpression::NameContext::Global>(value.GetExpression<MidoriExpression::NameAccess>().m_name_ctx))
	{
		const std::unordered_map<std::string, MidoriExpression::Function*>::const_iterator named = m_module_functions.find(value.GetExpression<MidoriExpression::NameAccess>().m_name.m_lexeme);
		function = named != m_module_functions.cend() ? named->second : nullptr;
	}

	if (function == nullptr)
	{
		return;
	}

	const CarriedCells carried = CarriedCellFinder(m_module_functions).Find(*function);
	for (const Token& captured_name : carried.m_captured)
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, captured_name, CellCrossesWorkerMessage);
	}
	for (const Token& global_name : carried.m_globals)
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, token, std::format("The global reference '{}' is copied into the worker; writes on either side are not seen by the other.", global_name.m_lexeme));
	}
}

void CellCrossesWorkerDiagnostic::operator()(MidoriExpression::Send& send)
{
	MidoriAbstractSyntaxTreeWalker::operator()(send);
	WarnIfCell(*send.m_value, send.m_arrow);
	WarnIfFunctionCarriesCells(*send.m_value, send.m_arrow);
}

void CellCrossesWorkerDiagnostic::operator()(MidoriExpression::Call& call)
{
	MidoriAbstractSyntaxTreeWalker::operator()(call);

	const WorkerCrossing* crossing = FindCallCrossing(call, m_crossings);
	if (crossing == nullptr)
	{
		return;
	}

	const Token& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>().m_name;
	for (size_t index : crossing->m_crossing_parameters)
	{
		WarnIfCell(*call.m_arguments[index], callee);
		WarnIfFunctionCarriesCells(*call.m_arguments[index], callee);
	}
	if (crossing->m_returns_from_worker && ContainsCell(call.m_type_data))
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, callee, CellComesBackMessage);
	}
}
