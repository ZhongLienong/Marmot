#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>

namespace
{
	// Threading copies the block's instructions into the predecessor, so it
	// stays threading and never becomes general tail duplication.
	constexpr size_t s_max_inspection_size = 6u;

	using MarmotIRKnown = std::variant<int64_t, bool>;

	// A block that only looks at values and branches on what it sees: running
	// it again elsewhere changes nothing but where its values are.
	bool IsInspection(const MarmotIRBlock& block)
	{
		const std::span<const MarmotIRInstruction> body(block.m_instructions.data(), block.m_instructions.size() - 1u);
		return block.m_instructions.back().m_op == MarmotIROp::Branch
			&& body.size() <= s_max_inspection_size
			&& std::ranges::all_of(body, [](const MarmotIRInstruction& instruction) { return instruction.m_effect.m_kind == MarmotIREffectKind::Pure; });
	}

	std::optional<MarmotIRKnown> Compare(MarmotIROp op, const MarmotIRKnown& left, const MarmotIRKnown& right)
	{
		switch (op)
		{
		case MarmotIROp::EqInt:
		case MarmotIROp::EqBool:
			return left == right;
		case MarmotIROp::NeInt:
		case MarmotIROp::NeBool:
			return left != right;
		default:
			return std::nullopt;
		}
	}

	// Which successor the block's branch takes when it is entered with these
	// arguments, when the unions they were made as decide it.
	std::optional<size_t> DecidedSuccessor(const MarmotIRFunction& function, const std::vector<std::optional<MarmotIRSite>>& sites, const MarmotIRBlock& block, const std::vector<MarmotIRValueId>& arguments)
	{
		const auto passed = [&](MarmotIRValueId value)
		{
			const std::vector<MarmotIRValueId>::const_iterator parameter = std::ranges::find(block.m_parameters, value);
			return parameter == block.m_parameters.end() ? value : arguments[static_cast<size_t>(std::distance(block.m_parameters.begin(), parameter))];
		};
		std::unordered_map<uint32_t, MarmotIRKnown> known;
		const auto known_of = [&known](MarmotIRValueId value) -> std::optional<MarmotIRKnown>
		{
			const std::unordered_map<uint32_t, MarmotIRKnown>::const_iterator found = known.find(value.m_index);
			return found == known.end() ? std::nullopt : std::optional(found->second);
		};
		for (const MarmotIRInstruction& instruction : block.m_instructions | std::views::take(block.m_instructions.size() - 1u))
		{
			std::optional<MarmotIRKnown> value;
			switch (instruction.m_op)
			{
			case MarmotIROp::Const:
				if (std::holds_alternative<int64_t>(instruction.m_immediate))
				{
					value = std::get<int64_t>(instruction.m_immediate);
				}
				else if (std::holds_alternative<bool>(instruction.m_immediate))
				{
					value = std::get<bool>(instruction.m_immediate);
				}
				break;
			case MarmotIROp::GetTag:
			{
				const MarmotIRInstruction* whole = MarmotIRAnalysis::Definition(function, sites, passed(instruction.m_operands.front()));
				if (whole != nullptr && whole->m_op == MarmotIROp::MakeUnion)
				{
					value = static_cast<int64_t>(std::get<MarmotIRTag>(whole->m_immediate).m_value);
				}
				break;
			}
			case MarmotIROp::NotBool:
				value = known_of(instruction.m_operands.front()).transform([](const MarmotIRKnown& operand) { return MarmotIRKnown(!std::get<bool>(operand)); });
				break;
			default:
				if (instruction.m_operands.size() == 2u)
				{
					const std::optional<MarmotIRKnown> left = known_of(instruction.m_operands[0u]);
					const std::optional<MarmotIRKnown> right = known_of(instruction.m_operands[1u]);
					value = left.has_value() && right.has_value() ? Compare(instruction.m_op, left.value(), right.value()) : std::nullopt;
				}
				break;
			}
			if (value.has_value())
			{
				known.emplace(instruction.m_result->m_index, value.value());
			}
		}
		return known_of(block.m_instructions.back().m_operands.front()).transform([](const MarmotIRKnown& condition) { return std::get<bool>(condition) ? 0uz : 1uz; });
	}

	// The blocks reached from `from` without passing through `avoided`.
	std::vector<bool> ReachedAvoiding(const MarmotIRControlFlow& control_flow, size_t block_count, uint32_t from, uint32_t avoided)
	{
		std::vector<bool> reached(block_count, false);
		std::vector<uint32_t> pending = { from };
		reached[from] = true;
		while (!pending.empty())
		{
			const uint32_t block = pending.back();
			pending.pop_back();
			for (const uint32_t successor : control_flow.Successors(block))
			{
				if (successor != avoided && !reached[successor])
				{
					reached[successor] = true;
					pending.push_back(successor);
				}
			}
		}
		return reached;
	}

	// An edge into the block that inspects and a successor it would take.
	class Threading
	{
	private:
		MarmotIRFunction& m_function;
		const MarmotIRControlFlow& m_control_flow;
		MarmotIREdge m_edge;
		uint32_t m_inspection;
		uint32_t m_target;
		std::vector<bool> m_defined_by_inspection;

	public:
		Threading(MarmotIRFunction& function, const MarmotIRControlFlow& control_flow, MarmotIREdge edge, uint32_t inspection, uint32_t target)
			: m_function(function),
			m_control_flow(control_flow),
			m_edge(edge),
			m_inspection(inspection),
			m_target(target),
			m_defined_by_inspection(function.m_values.size(), false)
		{
			const MarmotIRBlock& block = function.m_blocks[inspection];
			std::ranges::for_each(block.m_parameters, [this](MarmotIRValueId parameter) { m_defined_by_inspection[parameter.m_index] = true; });
			for (const MarmotIRInstruction& instruction : block.m_instructions | std::views::take(block.m_instructions.size() - 1u))
			{
				m_defined_by_inspection[instruction.m_result->m_index] = true;
			}
		}

		// The target must be the inspecting block's alone, so that it can take
		// that block's values as parameters. A value of the inspecting block
		// read outside the target's blocks must stay behind that block: no
		// path from the target may reach the read around it.
		bool IsPossible() const
		{
			if (m_target == m_inspection || m_target == MarmotIRFunction::s_entry_block.m_index || m_edge.m_block == m_inspection)
			{
				return false;
			}
			const std::vector<MarmotIRSuccessor>& successors = m_function.m_blocks[m_inspection].m_instructions.back().m_successors;
			const bool only_from_inspection = m_control_flow.Predecessors(m_target) == std::vector<uint32_t>{ m_inspection }
				&& std::ranges::count_if(successors, [this](const MarmotIRSuccessor& successor) { return successor.m_block.m_index == m_target; }) == 1;
			if (!only_from_inspection)
			{
				return false;
			}
			const std::vector<bool> reached = ReachedAvoiding(m_control_flow, m_function.m_blocks.size(), m_target, m_inspection);
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (block == m_inspection || !reached[block] || m_control_flow.Dominates(m_target, block) || !ReadsInspection(block))
				{
					continue;
				}
				return false;
			}
			return true;
		}

		void Apply()
		{
			const std::vector<MarmotIRValueId> passed = MarmotIRAnalysis::SuccessorOf(m_function, m_edge).m_arguments;
			MarmotIRBlock& inspection = m_function.m_blocks[m_inspection];
			const size_t decided = static_cast<size_t>(std::ranges::find_if(inspection.m_instructions.back().m_successors, [this](const MarmotIRSuccessor& successor) { return successor.m_block.m_index == m_target; }) - inspection.m_instructions.back().m_successors.begin());

			std::vector<std::optional<MarmotIRValueId>> copied(m_function.m_values.size());
			for (size_t index = 0u; index < inspection.m_parameters.size(); index += 1u)
			{
				copied[inspection.m_parameters[index].m_index] = passed[index];
			}
			const auto copy_of = [&copied](MarmotIRValueId value) { return copied[value.m_index].value_or(value); };
			std::vector<MarmotIRInstruction> copies;
			for (const MarmotIRInstruction& instruction : inspection.m_instructions | std::views::take(inspection.m_instructions.size() - 1u))
			{
				MarmotIRInstruction copy = instruction;
				std::ranges::for_each(copy.m_operands, [&copy_of](MarmotIRValueId& operand) { operand = copy_of(operand); });
				const MarmotIRValue& original = m_function.Value(instruction.m_result.value());
				copy.m_result = MarmotIRAnalysis::AddValue(m_function, original.m_type, original.m_name);
				copied.push_back(std::nullopt);
				copied[instruction.m_result->m_index] = copy.m_result;
				copies.push_back(std::move(copy));
			}

			const std::vector<MarmotIRValueId> carried = CarriedValues();
			std::vector<std::optional<MarmotIRValueId>> renamed(m_function.m_values.size());
			for (const MarmotIRValueId value : carried)
			{
				const MarmotIRValue& original = m_function.Value(value);
				const MarmotIRValueId parameter = MarmotIRAnalysis::AddValue(m_function, original.m_type, original.m_name);
				m_function.m_blocks[m_target].m_parameters.push_back(parameter);
				renamed[value.m_index] = parameter;
			}

			std::vector<MarmotIRValueId>& kept = m_function.m_blocks[m_inspection].m_instructions.back().m_successors[decided].m_arguments;
			std::vector<MarmotIRValueId> threaded = kept | std::views::transform(copy_of) | std::ranges::to<std::vector>();
			std::ranges::copy(carried, std::back_inserter(kept));
			std::ranges::copy(carried | std::views::transform(copy_of), std::back_inserter(threaded));

			std::vector<MarmotIRInstruction>& predecessor = m_function.m_blocks[m_edge.m_block].m_instructions;
			predecessor.insert(predecessor.end() - 1, std::make_move_iterator(copies.begin()), std::make_move_iterator(copies.end()));
			MarmotIRAnalysis::SuccessorOf(m_function, m_edge) = MarmotIRSuccessor(MarmotIRBlockId{ m_target }, std::move(threaded));

			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_control_flow.Dominates(m_target, block))
				{
					continue;
				}
				for (MarmotIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					ForEachMarmotIRUse(instruction, [&renamed](MarmotIRValueId& value)
					{
						value = value.m_index < renamed.size() ? renamed[value.m_index].value_or(value) : value;
					});
				}
			}
		}

	private:
		bool ReadsInspection(uint32_t block) const
		{
			return std::ranges::any_of(m_function.m_blocks[block].m_instructions, [this](const MarmotIRInstruction& instruction)
			{
				bool reads = false;
				ForEachMarmotIRUse(instruction, [this, &reads](MarmotIRValueId value) { reads = reads || m_defined_by_inspection[value.m_index]; });
				return reads;
			});
		}

		// The inspecting block's values that the target's blocks read, which
		// the target takes as parameters: that block no longer dominates it.
		std::vector<MarmotIRValueId> CarriedValues() const
		{
			std::vector<bool> read(m_defined_by_inspection.size(), false);
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_control_flow.Dominates(m_target, block))
				{
					continue;
				}
				for (const MarmotIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					ForEachMarmotIRUse(instruction, [this, &read](MarmotIRValueId value)
					{
						read[value.m_index] = read[value.m_index] || m_defined_by_inspection[value.m_index];
					});
				}
			}
			const MarmotIRBlock& inspection = m_function.m_blocks[m_inspection];
			std::vector<MarmotIRValueId> defined = inspection.m_parameters;
			std::ranges::copy(inspection.m_instructions | std::views::take(inspection.m_instructions.size() - 1u) | std::views::transform([](const MarmotIRInstruction& instruction) { return instruction.m_result.value(); }), std::back_inserter(defined));
			return defined | std::views::filter([&read](MarmotIRValueId value) { return read[value.m_index]; }) | std::ranges::to<std::vector>();
		}
	};

	bool ThreadOneEdge(MarmotIRFunction& function)
	{
		MarmotIRAnalysis::RemoveUnreachableBlocks(function);
		const MarmotIRControlFlow control_flow(function);
		const std::vector<std::optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
		for (uint32_t inspection = 1u; inspection < function.m_blocks.size(); inspection += 1u)
		{
			const MarmotIRBlock& block = function.m_blocks[inspection];
			if (!IsInspection(block))
			{
				continue;
			}
			for (const uint32_t predecessor : control_flow.Predecessors(inspection))
			{
				const std::vector<MarmotIRSuccessor>& successors = function.m_blocks[predecessor].m_instructions.back().m_successors;
				for (size_t successor = 0u; successor < successors.size(); successor += 1u)
				{
					if (successors[successor].m_block.m_index != inspection)
					{
						continue;
					}
					const std::optional<size_t> decided = DecidedSuccessor(function, sites, block, successors[successor].m_arguments);
					if (!decided.has_value())
					{
						continue;
					}
					Threading threading(function, control_flow, MarmotIREdge{ predecessor, successor }, inspection, block.m_instructions.back().m_successors[decided.value()].m_block.m_index);
					if (threading.IsPossible())
					{
						threading.Apply();
						return true;
					}
				}
			}
		}
		return false;
	}

	bool ThreadKnownConstructors(MarmotIRFunction& function)
	{
		const size_t blocks_before = function.m_blocks.size();
		bool changed = false;
		// One edge per round, since each threading changes the graph the next
		// is found in. An edge only moves forward, to a block that had one
		// predecessor; the bound caps the rounds a function with many
		// decided branches takes.
		const size_t limit = 4u * function.m_blocks.size();
		for (size_t round = 0u; round < limit && ThreadOneEdge(function); round += 1u)
		{
			changed = true;
		}
		return changed || function.m_blocks.size() != blocks_before;
	}
}

bool RunKnownConstructorThreading(MarmotIRModule& module)
{
	return MarmotIRAnalysis::TransformFunctions(module, ThreadKnownConstructors);
}
