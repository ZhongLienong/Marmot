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

	// By a local's place in the one sequence that numbers a function's locals and
	// those of the functions around it: a Local index counts from its function's
	// captured count, and a Cell index is the place itself.
	using LocalOrigins = std::unordered_map<int, Origins>;

	// Everything an expression reads, which over-approximates where its value came
	// from: a lambda's value is what it captured.
	class OriginCollector final : public MidoriAbstractSyntaxTreeWalker
	{
	public:
		OriginCollector(const LocalOrigins& locals, const WorkerCrossings& crossings, int captured_count)
			: m_locals(locals),
			m_crossings(crossings),
			m_captured_counts{ captured_count }
		{
		}

		Origins Collect(MidoriExpression& expression)
		{
			VisitExpression(expression);
			return std::move(m_origins);
		}

	protected:
		void operator()(MidoriExpression::NameAccess& access) override
		{
			const MidoriExpression::NameContext::Local* local = std::get_if<MidoriExpression::NameContext::Local>(&access.m_name_ctx);
			const MidoriExpression::NameContext::Cell* cell = std::get_if<MidoriExpression::NameContext::Cell>(&access.m_name_ctx);
			if (local == nullptr && cell == nullptr)
			{
				return;
			}

			const LocalOrigins::const_iterator found = m_locals.find(local != nullptr ? m_captured_counts.back() + local->m_index : cell->m_index);
			if (found != m_locals.cend())
			{
				m_origins |= found->second;
			}
		}

		void operator()(MidoriExpression::Function& function) override
		{
			m_captured_counts.push_back(function.m_captured_count);
			MidoriAbstractSyntaxTreeWalker::operator()(function);
			m_captured_counts.pop_back();
		}

		void operator()(MidoriExpression::Call& call) override
		{
			MidoriAbstractSyntaxTreeWalker::operator()(call);
			const WorkerCrossing* crossing = FindCallCrossing(call, m_crossings);
			m_origins.m_from_worker = m_origins.m_from_worker || (crossing != nullptr && crossing->m_returns_from_worker);
		}

	private:
		const LocalOrigins& m_locals;
		const WorkerCrossings& m_crossings;
		std::vector<int> m_captured_counts;
		Origins m_origins;
	};

	// One function's summary. A parameter crosses when an expression that reads
	// it, or a local that came from it, is an argument a call hands to a worker or
	// a value sent through a channel; a read inside a lambda counts, since the
	// lambda carries it.
	class SummaryFinder final : public MidoriAbstractSyntaxTreeWalker
	{
	public:
		explicit SummaryFinder(const WorkerCrossings& crossings)
			: m_crossings(crossings)
		{
		}

		// A parameter whose declared type already holds a cell was warned about
		// inside the function, where it crossed, so it is left out. A function
		// stays in: what a function captured is not in its type.
		WorkerCrossing Find(MidoriExpression::Function& function)
		{
			m_captured_counts.push_back(function.m_captured_count);
			for (size_t index = 0u; index < function.m_params.size(); index += 1u)
			{
				m_locals.insert_or_assign(Place(static_cast<int>(index)), Origins{ { index }, false });
			}
			VisitExpression(function.m_body);
			const bool returns_from_worker = OriginsOf(*function.m_body).m_from_worker;
			m_captured_counts.pop_back();

			const MidoriType::FunctionType& signature = function.m_type_data->GetType<MidoriType::FunctionType>();
			WorkerCrossing crossing;
			crossing.m_crossing_parameters = m_crossing_parameters
				| std::views::filter([&signature](size_t index)
				{
					const std::shared_ptr<MidoriType>& parameter = signature.m_param_types[index];
					return parameter->IsType<MidoriType::FunctionType>() || !ContainsCell(parameter);
				})
				| std::ranges::to<std::vector>();
			crossing.m_returns_from_worker = returns_from_worker && !ContainsCell(signature.m_return_type);
			return crossing;
		}

	protected:
		void operator()(MidoriStatement::VariableDefinition& definition) override
		{
			MidoriAbstractSyntaxTreeWalker::operator()(definition);
			if (definition.m_local_index.has_value())
			{
				m_locals.insert_or_assign(Place(definition.m_local_index.value()), OriginsOf(*definition.m_value));
			}
		}

		void operator()(MidoriStatement::TupleDefinition& definition) override
		{
			MidoriAbstractSyntaxTreeWalker::operator()(definition);
			const Origins origins = OriginsOf(*definition.m_value);
			for (const std::optional<int>& local_index : definition.m_local_indices)
			{
				if (local_index.has_value())
				{
					m_locals.insert_or_assign(Place(local_index.value()), origins);
				}
			}
		}

		void operator()(MidoriPattern::Binding& binding) override
		{
			if (binding.m_local_index.has_value())
			{
				m_locals.insert_or_assign(Place(binding.m_local_index.value()), m_scrutinees.back());
			}
		}

		void operator()(MidoriExpression::Match& match) override
		{
			VisitExpression(match.m_arg_expr);
			m_scrutinees.push_back(OriginsOf(*match.m_arg_expr));
			for (std::unique_ptr<MidoriExpression>& case_expr : match.m_cases)
			{
				VisitExpression(case_expr);
			}
			m_scrutinees.pop_back();
		}

		void operator()(MidoriExpression::For& for_expr) override
		{
			VisitExpression(for_expr.m_range);
			m_locals.insert_or_assign(Place(for_expr.m_loop_variable_index), OriginsOf(*for_expr.m_range));
			VisitExpression(for_expr.m_body);
		}

		void operator()(MidoriExpression::ArrayComprehension& comprehension) override
		{
			VisitExpression(comprehension.m_range);
			m_locals.insert_or_assign(Place(comprehension.m_loop_variable_index), OriginsOf(*comprehension.m_range));
			VisitExpression(comprehension.m_transform_expr);
		}

		void operator()(MidoriExpression::Function& function) override
		{
			m_captured_counts.push_back(function.m_captured_count);
			MidoriAbstractSyntaxTreeWalker::operator()(function);
			m_captured_counts.pop_back();
		}

		void operator()(MidoriExpression::Call& call) override
		{
			MidoriAbstractSyntaxTreeWalker::operator()(call);
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

		void operator()(MidoriExpression::Send& send) override
		{
			MidoriAbstractSyntaxTreeWalker::operator()(send);
			Cross(OriginsOf(*send.m_value));
		}

	private:
		int Place(int local_index) const
		{
			return m_captured_counts.back() + local_index;
		}

		Origins OriginsOf(MidoriExpression& expression) const
		{
			return OriginCollector(m_locals, m_crossings, m_captured_counts.back()).Collect(expression);
		}

		void Cross(const Origins& origins)
		{
			m_crossing_parameters.insert(origins.m_parameters.cbegin(), origins.m_parameters.cend());
		}

		const WorkerCrossings& m_crossings;
		LocalOrigins m_locals;
		std::vector<int> m_captured_counts;
		std::vector<Origins> m_scrutinees;
		std::set<size_t> m_crossing_parameters;
	};

	bool HoldsCell(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited)
	{
		if (type == nullptr || !visited.insert(type.get()).second)
		{
			return false;
		}

		const std::function<bool(const std::shared_ptr<MidoriType>&)> contains = [&visited](const std::shared_ptr<MidoriType>& part)
		{
			return HoldsCell(part, visited);
		};
		if (type->IsType<MidoriType::CellType>())
		{
			return true;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			return contains(type->GetType<MidoriType::ArrayType>().m_element_type);
		}
		if (type->IsType<MidoriType::ChannelType>())
		{
			return contains(type->GetType<MidoriType::ChannelType>().m_element_type);
		}
		if (type->IsType<MidoriType::TupleType>())
		{
			return std::ranges::any_of(type->GetType<MidoriType::TupleType>().m_element_types, contains);
		}
		if (type->IsType<MidoriType::StructType>())
		{
			return std::ranges::any_of(type->GetType<MidoriType::StructType>().m_member_types, contains);
		}
		if (type->IsType<MidoriType::UnionType>())
		{
			return std::ranges::any_of(type->GetType<MidoriType::UnionType>().m_member_info, [&contains](const std::pair<const std::string, MidoriType::UnionType::UnionMemberContext>& member)
			{
				return std::ranges::any_of(member.second.m_member_types, contains);
			});
		}
		return false;
	}
}

const WorkerCrossing* FindCallCrossing(const MidoriExpression::Call& call, const WorkerCrossings& crossings)
{
	static const WorkerCrossing s_spawn{ { 0u, 1u }, false };
	static const WorkerCrossing s_join{ {}, true };
	if (call.m_primitive == MidoriPrimitive::Spawn)
	{
		return &s_spawn;
	}
	if (call.m_primitive == MidoriPrimitive::Join)
	{
		return &s_join;
	}
	if (!call.m_callee->IsExpression<MidoriExpression::NameAccess>())
	{
		return nullptr;
	}

	const MidoriExpression::NameAccess& callee = call.m_callee->GetExpression<MidoriExpression::NameAccess>();
	if (!std::holds_alternative<MidoriExpression::NameContext::Global>(callee.m_name_ctx))
	{
		return nullptr;
	}
	const WorkerCrossings::const_iterator found = crossings.find(callee.m_name.m_lexeme);
	return found != crossings.cend() ? &found->second : nullptr;
}

// Each function's summary depends on those of the functions it calls, which may
// call it back, so all are found again until none changes. They only grow.
WorkerCrossings FindWorkerCrossings(MidoriProgramTree& program_tree, const WorkerCrossings& imported)
{
	std::vector<std::pair<std::string, MidoriExpression::Function*>> functions;
	for (std::unique_ptr<MidoriStatement>& statement : program_tree)
	{
		if (!statement->IsStatement<MidoriStatement::VariableDefinition>())
		{
			continue;
		}
		MidoriStatement::VariableDefinition& definition = statement->GetStatement<MidoriStatement::VariableDefinition>();
		if (definition.m_value->IsExpression<MidoriExpression::Function>())
		{
			functions.emplace_back(definition.m_name.m_lexeme, &definition.m_value->GetExpression<MidoriExpression::Function>());
		}
	}

	WorkerCrossings crossings = imported;
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const std::pair<std::string, MidoriExpression::Function*>& function : functions)
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
		| std::views::filter([&crossings](const std::pair<std::string, MidoriExpression::Function*>& function) { return crossings.contains(function.first); })
		| std::views::transform([&crossings](const std::pair<std::string, MidoriExpression::Function*>& function) { return std::pair<const std::string, WorkerCrossing>(function.first, crossings.at(function.first)); })
		| std::ranges::to<WorkerCrossings>();
}

bool ContainsCell(const std::shared_ptr<MidoriType>& type)
{
	std::unordered_set<const MidoriType*> visited;
	return HoldsCell(type, visited);
}
