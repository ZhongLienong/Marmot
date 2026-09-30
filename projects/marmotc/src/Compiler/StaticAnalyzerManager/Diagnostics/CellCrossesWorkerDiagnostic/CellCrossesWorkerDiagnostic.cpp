#include "CellCrossesWorkerDiagnostic.h"

#include <utility>
#include <variant>
#include <vector>

namespace
{
	constexpr std::string_view CellCrossesWorkerMessage = "This Cell is copied into the worker; writes on either side are not seen by the other.";
	constexpr std::string_view ParallelMapName = "Concurrency::ParallelMap";

	// Collects the names a spawned lambda captures whose type holds a cell. Only the
	// spawned lambda's own captures cross to the worker: a lambda nested inside it
	// may capture the worker's own locals, which never leave, so nested lambdas are
	// not entered.
	class CapturedCellFinder final : public MidoriAbstractSyntaxTreeWalker
	{
	public:
		std::vector<Token> Find(std::unique_ptr<MidoriExpression>& body)
		{
			VisitExpression(body);
			return std::move(m_found);
		}

	protected:
		void operator()(MidoriExpression::Function&) override
		{
		}

		void operator()(MidoriExpression::NameAccess& access) override
		{
			const MidoriExpression::NameContext::Cell* captured = std::get_if<MidoriExpression::NameContext::Cell>(&access.m_name_ctx);
			if (captured != nullptr && CellCrossesWorkerDiagnostic::ContainsCell(access.m_type_data) && m_seen.insert(captured->m_index).second)
			{
				m_found.emplace_back(access.m_name);
			}
		}

	private:
		std::vector<Token> m_found;
		std::unordered_set<int> m_seen;
	};
}

std::string_view CellCrossesWorkerDiagnostic::GetName() const
{
	return "CellCrossesWorkerDiagnostic";
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

void CellCrossesWorkerDiagnostic::operator()(MidoriExpression::Spawn& spawn)
{
	MidoriAbstractSyntaxTreeWalker::operator()(spawn);

	for (const std::unique_ptr<MidoriExpression>& argument : spawn.m_arguments)
	{
		WarnIfCell(*argument, spawn.m_spawn_keyword);
	}

	if (spawn.m_callee->IsExpression<MidoriExpression::Function>())
	{
		MidoriExpression::Function& function = spawn.m_callee->GetExpression<MidoriExpression::Function>();
		CapturedCellFinder finder;
		for (const Token& captured_name : finder.Find(function.m_body))
		{
			EmitWarning(CompilerWarningCode::CellCrossesWorker, captured_name, CellCrossesWorkerMessage);
		}
	}
}

void CellCrossesWorkerDiagnostic::operator()(MidoriExpression::Send& send)
{
	MidoriAbstractSyntaxTreeWalker::operator()(send);
	WarnIfCell(*send.m_value, send.m_arrow);
}

void CellCrossesWorkerDiagnostic::operator()(MidoriExpression::Call& call)
{
	MidoriAbstractSyntaxTreeWalker::operator()(call);

	if (!call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return;
	}

	const MidoriExpression::NameAccess& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
	if (callee.m_name.m_lexeme != ParallelMapName || call.m_arguments.empty())
	{
		return;
	}

	if (ContainsCell(call.m_arguments[0u]->GetType()) || ContainsCell(call.m_type_data))
	{
		EmitWarning(CompilerWarningCode::CellCrossesWorker, call.m_paren, CellCrossesWorkerMessage);
	}
}
