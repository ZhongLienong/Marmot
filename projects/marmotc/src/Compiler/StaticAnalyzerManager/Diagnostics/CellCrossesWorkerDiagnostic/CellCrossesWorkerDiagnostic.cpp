#include "CellCrossesWorkerDiagnostic.h"

#include <format>
#include <utility>
#include <variant>
#include <vector>

namespace
{
	constexpr std::string_view CellCrossesWorkerMessage = "This reference is copied into the worker; writes on either side are not seen by the other.";
	constexpr std::string_view ParallelMapName = "Concurrency::ParallelMap";

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
				if (captured->m_index < m_captured_count && CellCrossesWorkerDiagnostic::ContainsCell(access.m_type_data) && m_seen_cells.insert(captured->m_index).second)
				{
					m_found.m_captured.emplace_back(access.m_name);
				}
				return;
			}

			if (!std::holds_alternative<MidoriExpression::NameContext::Global>(access.m_name_ctx))
			{
				return;
			}

			if (CellCrossesWorkerDiagnostic::ContainsCell(access.m_type_data))
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

bool CellCrossesWorkerDiagnostic::ContainsCell(const std::shared_ptr<MidoriType>& type)
{
	std::unordered_set<const MidoriType*> visited;
	return ContainsCell(type, visited);
}

bool CellCrossesWorkerDiagnostic::ContainsCell(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
{
	if (type == nullptr || !visited.insert(type.get()).second)
	{
		return false;
	}

	if (type->IsType<MidoriType::CellType>())
	{
		return true;
	}
	if (type->IsType<MidoriType::ArrayType>())
	{
		return ContainsCell(type->GetType<MidoriType::ArrayType>().m_element_type, visited);
	}
	if (type->IsType<MidoriType::ChannelType>())
	{
		return ContainsCell(type->GetType<MidoriType::ChannelType>().m_element_type, visited);
	}
	if (type->IsType<MidoriType::TupleType>())
	{
		for (const std::shared_ptr<MidoriType>& element_type : type->GetType<MidoriType::TupleType>().m_element_types)
		{
			if (ContainsCell(element_type, visited))
			{
				return true;
			}
		}
		return false;
	}
	if (type->IsType<MidoriType::StructType>())
	{
		for (const std::shared_ptr<MidoriType>& member_type : type->GetType<MidoriType::StructType>().m_member_types)
		{
			if (ContainsCell(member_type, visited))
			{
				return true;
			}
		}
		return false;
	}
	if (type->IsType<MidoriType::UnionType>())
	{
		for (const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member : type->GetType<MidoriType::UnionType>().m_member_info)
		{
			for (const std::shared_ptr<MidoriType>& member_type : member.second.m_member_types)
			{
				if (ContainsCell(member_type, visited))
				{
					return true;
				}
			}
		}
		return false;
	}

	return false;
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

	if (!call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return;
	}

	const MidoriExpression::NameAccess& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
	if (call.m_primitive == MidoriPrimitive::Spawn)
	{
		WarnIfCell(*call.m_arguments[0u], callee.m_name);
		WarnIfFunctionCarriesCells(*call.m_arguments[1u], callee.m_name);
		return;
	}

	if (callee.m_name.m_lexeme != ParallelMapName || call.m_arguments.size() < 2u)
	{
		return;
	}

	if (ContainsCell(call.m_arguments[0u]->GetType()) || ContainsCell(call.m_type_data))
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, call.m_paren, CellCrossesWorkerMessage);
	}
	WarnIfFunctionCarriesCells(*call.m_arguments[1u], call.m_paren);
}
