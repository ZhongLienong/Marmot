#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// An edge into a block: the terminator of `m_block` and which of its
	// successors it is.
	struct Edge
	{
		uint32_t m_block;
		size_t m_successor;
	};

	std::vector<std::vector<Edge>> IncomingEdges(const MidoriIRFunction& function)
	{
		std::vector<std::vector<Edge>> incoming(function.m_blocks.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MidoriIRSuccessor>& successors = function.m_blocks[block].m_instructions.back().m_successors;
			for (size_t successor = 0u; successor < successors.size(); successor += 1u)
			{
				incoming[successors[successor].m_block.m_index].push_back(Edge{ block, successor });
			}
		}
		return incoming;
	}

	MidoriIRSuccessor& SuccessorOf(MidoriIRFunction& function, const Edge& edge)
	{
		return function.m_blocks[edge.m_block].m_instructions.back().m_successors[edge.m_successor];
	}

	// A parameter every edge passes the same value to, or itself, is that
	// value. It dominates each edge, so it dominates the block.
	bool RemoveRedundantParameters(MidoriIRFunction& function)
	{
		const std::vector<std::vector<Edge>> incoming = IncomingEdges(function);
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		bool changed = false;
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			std::vector<MidoriIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = parameters.size(); index-- > 0u;)
			{
				const MidoriIRValueId parameter = parameters[index];
				std::optional<MidoriIRValueId> only;
				bool is_redundant = true;
				for (const Edge& edge : incoming[block])
				{
					const MidoriIRValueId argument = SuccessorOf(function, edge).m_arguments[index];
					if (argument == parameter || argument == only)
					{
						continue;
					}
					if (only.has_value())
					{
						is_redundant = false;
						break;
					}
					only = argument;
				}
				if (!is_redundant || !only.has_value())
				{
					continue;
				}
				replacements[parameter.m_index] = only;
				parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
				for (const Edge& edge : incoming[block])
				{
					std::vector<MidoriIRValueId>& arguments = SuccessorOf(function, edge).m_arguments;
					arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				}
				changed = true;
			}
		}
		if (changed)
		{
			MidoriIRAnalysis::ReplaceUses(function, replacements);
		}
		return changed;
	}

	// A value is live when an instruction that must run reads it, or a live
	// instruction or live parameter needs it; a parameter's arguments are
	// live when it is.
	bool RemoveDeadValues(MidoriIRFunction& function)
	{
		const std::vector<std::optional<MidoriIRSite>> sites = MidoriIRAnalysis::DefinitionSites(function);
		const std::vector<std::vector<Edge>> incoming = IncomingEdges(function);
		std::vector<std::optional<std::pair<uint32_t, size_t>>> parameter_of(function.m_values.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MidoriIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = 0u; index < parameters.size(); index += 1u)
			{
				parameter_of[parameters[index].m_index] = std::pair{ block, index };
			}
		}

		std::vector<bool> live(function.m_values.size(), false);
		std::vector<MidoriIRValueId> pending;
		const auto mark = [&](MidoriIRValueId value)
		{
			if (!live[value.m_index])
			{
				live[value.m_index] = true;
				pending.push_back(value);
			}
		};
		for (const MidoriIRBlock& block : function.m_blocks)
		{
			for (const MidoriIRInstruction& instruction : block.m_instructions)
			{
				if (IsMidoriIRTerminator(instruction.m_op) || !MidoriIRAnalysis::IsRemovable(function, sites, instruction))
				{
					std::ranges::for_each(instruction.m_operands, mark);
				}
			}
		}
		while (!pending.empty())
		{
			const MidoriIRValueId value = pending.back();
			pending.pop_back();
			const MidoriIRInstruction* definition = MidoriIRAnalysis::Definition(function, sites, value);
			if (definition != nullptr)
			{
				std::ranges::for_each(definition->m_operands, mark);
			}
			else if (parameter_of[value.m_index].has_value())
			{
				const auto [block, index] = parameter_of[value.m_index].value();
				for (const Edge& edge : incoming[block])
				{
					mark(SuccessorOf(function, edge).m_arguments[index]);
				}
			}
		}

		bool changed = false;
		for (MidoriIRBlock& block : function.m_blocks)
		{
			const size_t before = block.m_instructions.size();
			std::erase_if(block.m_instructions, [&](const MidoriIRInstruction& instruction)
			{
				return instruction.m_result.has_value() && !live[instruction.m_result->m_index] && MidoriIRAnalysis::IsRemovable(function, sites, instruction);
			});
			changed = changed || block.m_instructions.size() != before;
		}
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			std::vector<MidoriIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = parameters.size(); index-- > 0u;)
			{
				if (live[parameters[index].m_index])
				{
					continue;
				}
				parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
				for (const Edge& edge : incoming[block])
				{
					std::vector<MidoriIRValueId>& arguments = SuccessorOf(function, edge).m_arguments;
					arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				}
				changed = true;
			}
		}
		return changed;
	}

	std::optional<bool> ConstantCondition(const MidoriIRFunction& function, const std::vector<std::optional<MidoriIRSite>>& sites, MidoriIRValueId condition)
	{
		const MidoriIRInstruction* definition = MidoriIRAnalysis::Definition(function, sites, condition);
		if (definition == nullptr || definition->m_op != MidoriIROp::Const)
		{
			return std::nullopt;
		}
		return std::get<bool>(definition->m_immediate);
	}

	bool SameSuccessor(const MidoriIRSuccessor& left, const MidoriIRSuccessor& right)
	{
		return left.m_block == right.m_block && left.m_arguments == right.m_arguments;
	}

	// A branch on a constant, or whose two edges are one, is a jump.
	bool FoldBranches(MidoriIRFunction& function)
	{
		const std::vector<std::optional<MidoriIRSite>> sites = MidoriIRAnalysis::DefinitionSites(function);
		bool changed = false;
		for (MidoriIRBlock& block : function.m_blocks)
		{
			MidoriIRInstruction& terminator = block.m_instructions.back();
			if (terminator.m_op != MidoriIROp::Branch)
			{
				continue;
			}
			const std::optional<bool> condition = ConstantCondition(function, sites, terminator.m_operands.front());
			if (!condition.has_value() && !SameSuccessor(terminator.m_successors[0u], terminator.m_successors[1u]))
			{
				continue;
			}
			MidoriIRSuccessor taken = std::move(terminator.m_successors[condition.value_or(true) ? 0u : 1u]);
			terminator.m_op = MidoriIROp::Jump;
			terminator.m_operands.clear();
			terminator.m_successors.clear();
			terminator.m_successors.push_back(std::move(taken));
			changed = true;
		}
		return changed;
	}

	// An edge into a block that only jumps on goes where that block goes,
	// passing what it would have: the block's parameters are the edge's
	// arguments, and any other value dominates the block, so it dominates
	// every edge into it. Not when a block it dominates uses its parameters
	// directly, which would then have no definition on the way there.
	bool SkipForwardingBlocks(MidoriIRFunction& function)
	{
		std::vector<uint32_t> uses = MidoriIRAnalysis::CountUses(function);
		bool changed = false;
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			const MidoriIRBlock& forwarding = function.m_blocks[block];
			if (forwarding.m_instructions.size() != 1u || forwarding.m_instructions.front().m_op != MidoriIROp::Jump)
			{
				continue;
			}
			const std::vector<MidoriIRValueId>& passed = forwarding.m_instructions.front().m_successors.front().m_arguments;
			const bool uses_only_to_pass_on = std::ranges::all_of(forwarding.m_parameters, [&](MidoriIRValueId parameter)
			{
				return uses[parameter.m_index] == static_cast<uint32_t>(std::ranges::count(passed, parameter));
			});
			if (!uses_only_to_pass_on)
			{
				continue;
			}
			const MidoriIRSuccessor& onward = forwarding.m_instructions.front().m_successors.front();
			if (onward.m_block.m_index == block)
			{
				continue;
			}
			for (MidoriIRBlock& predecessor : function.m_blocks)
			{
				for (MidoriIRSuccessor& successor : predecessor.m_instructions.back().m_successors)
				{
					if (successor.m_block.m_index != block || &predecessor == &forwarding)
					{
						continue;
					}
					std::vector<MidoriIRValueId> arguments = onward.m_arguments
						| std::views::transform([&](MidoriIRValueId argument)
						{
							const std::vector<MidoriIRValueId>::const_iterator parameter = std::ranges::find(forwarding.m_parameters, argument);
							return parameter == forwarding.m_parameters.end() ? argument : successor.m_arguments[static_cast<size_t>(std::distance(forwarding.m_parameters.begin(), parameter))];
						})
						| std::ranges::to<std::vector>();
					successor = MidoriIRSuccessor(onward.m_block, std::move(arguments));
					changed = true;
				}
			}
			uses = MidoriIRAnalysis::CountUses(function);
		}
		return changed;
	}

	// A block whose only predecessor jumps to it joins that predecessor.
	bool MergeBlocks(MidoriIRFunction& function)
	{
		const MidoriIRControlFlow control_flow(function);
		std::vector<size_t> predecessor_counts(function.m_blocks.size(), 0u);
		for (const MidoriIRBlock& block : function.m_blocks)
		{
			for (const MidoriIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				predecessor_counts[successor.m_block.m_index] += 1u;
			}
		}

		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		bool changed = false;
		for (const uint32_t block : control_flow.ReversePostorder())
		{
			while (true)
			{
				MidoriIRBlock& predecessor = function.m_blocks[block];
				const MidoriIRInstruction& jump = predecessor.m_instructions.back();
				if (jump.m_op != MidoriIROp::Jump)
				{
					break;
				}
				const uint32_t target = jump.m_successors.front().m_block.m_index;
				if (target == block || target == MidoriIRFunction::s_entry_block.m_index || predecessor_counts[target] != 1u)
				{
					break;
				}
				MidoriIRBlock& merged = function.m_blocks[target];
				for (size_t index = 0u; index < merged.m_parameters.size(); index += 1u)
				{
					replacements[merged.m_parameters[index].m_index] = jump.m_successors.front().m_arguments[index];
				}
				merged.m_parameters.clear();
				predecessor.m_instructions.pop_back();
				std::ranges::move(merged.m_instructions, std::back_inserter(predecessor.m_instructions));
				// What is left does nothing and nothing reaches it.
				merged.m_instructions.clear();
				merged.m_instructions.push_back(MidoriIRAnalysis::Unreachable(0));
				predecessor_counts[target] = 0u;
				changed = true;
			}
		}
		if (changed)
		{
			MidoriIRAnalysis::ReplaceUses(function, replacements);
		}
		return changed;
	}

	void EliminateDeadCode(MidoriIRFunction& function)
	{
		// Each step only removes, so the rounds end; the bound is for a cycle
		// of blocks that only jump to each other, which never settles.
		for (size_t round = 0u; round <= function.m_blocks.size(); round += 1u)
		{
			MidoriIRAnalysis::RemoveUnreachableBlocks(function);
			bool changed = FoldBranches(function);
			changed = RemoveRedundantParameters(function) || changed;
			changed = RemoveDeadValues(function) || changed;
			changed = SkipForwardingBlocks(function) || changed;
			MidoriIRAnalysis::RemoveUnreachableBlocks(function);
			changed = MergeBlocks(function) || changed;
			if (!changed)
			{
				break;
			}
		}
		MidoriIRAnalysis::RemoveUnreachableBlocks(function);
	}
}

std::string_view DeadCodeEliminationPass::Name() const
{
	return "DeadCodeElimination";
}

void DeadCodeEliminationPass::Run(MidoriIRModule& module) const
{
	std::ranges::for_each(module.m_functions, EliminateDeadCode);
}
