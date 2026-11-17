#include "WorkerCrossing.h"

#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <set>
#include <unordered_set>
#include <utility>

namespace
{
	// Where a value may have come from: the parameters it may hold some of, and
	// whether a worker's result may be in it.
	struct Origins
	{
		std::set<size_t> m_parameters;
		bool m_from_worker = false;

		Origins& operator|=(const Origins& other)
		{
			m_parameters.insert(other.m_parameters.cbegin(), other.m_parameters.cend());
			m_from_worker = m_from_worker || other.m_from_worker;
			return *this;
		}
	};

	using LocalOrigins = std::unordered_map<BindingId, Origins>;

	// Everything an expression reads, which over-approximates where its value came
	// from: a lambda's value is what it captured.
	class OriginCollector final : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		OriginCollector(const LocalOrigins& locals, const WorkerCrossings& crossings)
			: m_locals(locals),
			m_crossings(crossings)
		{
		}

		Origins Collect(MarmotExpression& expression)
		{
			VisitExpression(expression);
			return std::move(m_origins);
		}

	protected:
		void operator()(MarmotExpression::NameAccess& access) override
		{
			const MarmotExpression::NameContext::Local* local = std::get_if<MarmotExpression::NameContext::Local>(&access.m_name_ctx);
			const MarmotExpression::NameContext::Cell* cell = std::get_if<MarmotExpression::NameContext::Cell>(&access.m_name_ctx);
			if (local == nullptr && cell == nullptr)
			{
				return;
			}

			const LocalOrigins::const_iterator found = m_locals.find(local != nullptr ? local->m_binding : cell->m_binding);
			if (found != m_locals.cend())
			{
				m_origins |= found->second;
			}
		}

		void operator()(MarmotExpression::Call& call) override
		{
			MarmotAbstractSyntaxTreeWalker::operator()(call);
			const WorkerCrossing* crossing = FindCallCrossing(call, m_crossings);
			m_origins.m_from_worker = m_origins.m_from_worker || (crossing != nullptr && crossing->m_returns_from_worker);
		}

	private:
		const LocalOrigins& m_locals;
		const WorkerCrossings& m_crossings;
		Origins m_origins;
	};

	// One function's summary. A parameter crosses when an expression that reads
	// it, or a local that came from it, is an argument a call hands to a worker or
	// a value sent through a channel; a read inside a lambda counts, since the
	// lambda carries it.
	class SummaryFinder final : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		explicit SummaryFinder(const WorkerCrossings& crossings)
			: m_crossings(crossings)
		{
		}

		// A parameter whose declared type already holds a cell was warned about
		// inside the function, where it crossed, so it is left out. A function
		// stays in: what a function captured is not in its type.
		WorkerCrossing Find(MarmotExpression::Function& function)
		{
			for (size_t index = 0u; index < function.m_params.Size(); index += 1u)
			{
				m_locals.insert_or_assign(function.m_params.BindingAt(index), Origins{ { index }, false });
			}
			VisitExpression(function.m_body);
			const bool returns_from_worker = OriginsOf(*function.m_body).m_from_worker;

			const MarmotType::FunctionType& signature = function.m_type_data->GetType<MarmotType::FunctionType>();
			WorkerCrossing crossing;
			crossing.m_crossing_parameters = m_crossing_parameters
				| std::views::filter([&signature](size_t index)
				{
					const std::shared_ptr<MarmotType>& parameter = signature.m_param_types[index];
					return parameter->IsType<MarmotType::FunctionType>() || !ContainsCell(parameter);
				})
				| std::ranges::to<std::vector>();
			crossing.m_returns_from_worker = returns_from_worker && !ContainsCell(signature.m_return_type);
			return crossing;
		}

	protected:
		void operator()(MarmotStatement::VariableDefinition& definition) override
		{
			MarmotAbstractSyntaxTreeWalker::operator()(definition);
			if (definition.m_binding.has_value())
			{
				m_locals.insert_or_assign(definition.m_binding.value(), OriginsOf(*definition.m_value));
			}
		}

		void operator()(MarmotStatement::TupleDefinition& definition) override
		{
			MarmotAbstractSyntaxTreeWalker::operator()(definition);
			const Origins origins = OriginsOf(*definition.m_value);
			for (const std::optional<BindingId>& binding : definition.m_bindings)
			{
				if (binding.has_value())
				{
					m_locals.insert_or_assign(binding.value(), origins);
				}
			}
		}

		void operator()(MarmotPattern::Binding& binding) override
		{
			if (binding.m_binding.has_value())
			{
				m_locals.insert_or_assign(binding.m_binding.value(), m_scrutinees.back());
			}
		}

		void operator()(MarmotExpression::Match& match) override
		{
			VisitExpression(match.m_arg_expr);
			m_scrutinees.push_back(OriginsOf(*match.m_arg_expr));
			for (std::unique_ptr<MarmotExpression>& case_expr : match.m_cases)
			{
				VisitExpression(case_expr);
			}
			m_scrutinees.pop_back();
		}

		void operator()(MarmotExpression::For& for_expr) override
		{
			VisitExpression(for_expr.m_range);
			m_locals.insert_or_assign(for_expr.m_loop_binding, OriginsOf(*for_expr.m_range));
			VisitExpression(for_expr.m_body);
		}

		void operator()(MarmotExpression::ArrayComprehension& comprehension) override
		{
			VisitExpression(comprehension.m_range);
			m_locals.insert_or_assign(comprehension.m_loop_binding, OriginsOf(*comprehension.m_range));
			VisitExpression(comprehension.m_transform_expr);
		}

		void operator()(MarmotExpression::Call& call) override
		{
			MarmotAbstractSyntaxTreeWalker::operator()(call);
			const WorkerCrossing* crossing = FindCallCrossing(call, m_crossings);
			if (crossing == nullptr)
			{
				return;
			}
			for (size_t index : crossing->m_crossing_parameters)
			{
				Cross(OriginsOf(*call.m_arguments[index]));
			}
		}

		void operator()(MarmotExpression::Send& send) override
		{
			MarmotAbstractSyntaxTreeWalker::operator()(send);
			Cross(OriginsOf(*send.m_value));
		}

	private:
		Origins OriginsOf(MarmotExpression& expression) const
		{
			return OriginCollector(m_locals, m_crossings).Collect(expression);
		}

		void Cross(const Origins& origins)
		{
			m_crossing_parameters.insert(origins.m_parameters.cbegin(), origins.m_parameters.cend());
		}

		const WorkerCrossings& m_crossings;
		LocalOrigins m_locals;
		std::vector<Origins> m_scrutinees;
		std::set<size_t> m_crossing_parameters;
	};

	bool HoldsCell(const std::shared_ptr<MarmotType>& type, std::unordered_set<const MarmotType*>& visited)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return false;
		}

		const std::function<bool(const std::shared_ptr<MarmotType>&)> contains = [&visited](const std::shared_ptr<MarmotType>& part)
		{
			return HoldsCell(part, visited);
		};
		if (type->IsType<MarmotType::CellType>())
		{
			return true;
		}
		if (type->IsType<MarmotType::ArrayType>())
		{
			return contains(type->GetType<MarmotType::ArrayType>().m_element_type);
		}
		if (type->IsType<MarmotType::ChannelType>())
		{
			return contains(type->GetType<MarmotType::ChannelType>().m_element_type);
		}
		if (type->IsType<MarmotType::TupleType>())
		{
			return std::ranges::any_of(type->GetType<MarmotType::TupleType>().m_element_types, contains);
		}
		if (type->IsType<MarmotType::StructType>())
		{
			return std::ranges::any_of(type->GetType<MarmotType::StructType>().m_member_types, contains);
		}
		if (type->IsType<MarmotType::UnionType>())
		{
			return std::ranges::any_of(type->GetType<MarmotType::UnionType>().m_member_info, [&contains](const std::pair<const std::string, MarmotType::UnionType::UnionMemberContext>& member)
			{
				return std::ranges::any_of(member.second.m_member_types, contains);
			});
		}
		return false;
	}
}

const WorkerCrossing* FindCallCrossing(const MarmotExpression::Call& call, const WorkerCrossings& crossings)
{
	static const WorkerCrossing s_spawn{ { 0u, 1u }, false };
	static const WorkerCrossing s_join{ {}, true };
	if (call.m_primitive == MarmotPrimitive::Spawn)
	{
		return &s_spawn;
	}
	if (call.m_primitive == MarmotPrimitive::Join)
	{
		return &s_join;
	}
	if (!call.m_callee->IsExpression<MarmotExpression::NameAccess>())
	{
		return nullptr;
	}

	const MarmotExpression::NameAccess& callee = call.m_callee->GetExpression<MarmotExpression::NameAccess>();
	if (!std::holds_alternative<MarmotExpression::NameContext::Global>(callee.m_name_ctx))
	{
		return nullptr;
	}
	const WorkerCrossings::const_iterator found = crossings.find(callee.m_name.m_lexeme);
	return found != crossings.cend() ? &found->second : nullptr;
}

// Each function's summary depends on those of the functions it calls, which may
// call it back, so all are found again until none changes. They only grow.
WorkerCrossings FindWorkerCrossings(MarmotProgramTree& program_tree, const WorkerCrossings& imported)
{
	std::vector<std::pair<std::string, MarmotExpression::Function*>> functions;
	for (std::unique_ptr<MarmotStatement>& statement : program_tree)
	{
		if (!statement->IsStatement<MarmotStatement::VariableDefinition>())
		{
			continue;
		}
		MarmotStatement::VariableDefinition& definition = statement->GetStatement<MarmotStatement::VariableDefinition>();
		if (definition.m_value->IsExpression<MarmotExpression::Function>())
		{
			functions.emplace_back(definition.m_name.m_lexeme, &definition.m_value->GetExpression<MarmotExpression::Function>());
		}
	}

	WorkerCrossings crossings = imported;
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const std::pair<std::string, MarmotExpression::Function*>& function : functions)
		{
			WorkerCrossing found = SummaryFinder(crossings).Find(*function.second);
			const bool hands_across = !found.m_crossing_parameters.empty() || found.m_returns_from_worker;
			const WorkerCrossings::const_iterator existing = crossings.find(function.first);
			if (hands_across && (existing == crossings.cend() || existing->second != found))
			{
				crossings.insert_or_assign(function.first, std::move(found));
				changed = true;
			}
		}
	}

	return functions
		| std::views::filter([&crossings](const std::pair<std::string, MarmotExpression::Function*>& function) { return crossings.contains(function.first); })
		| std::views::transform([&crossings](const std::pair<std::string, MarmotExpression::Function*>& function) { return std::pair<const std::string, WorkerCrossing>(function.first, crossings.at(function.first)); })
		| std::ranges::to<WorkerCrossings>();
}

bool ContainsCell(const std::shared_ptr<MarmotType>& type)
{
	std::unordered_set<const MarmotType*> visited;
	return HoldsCell(type, visited);
}
