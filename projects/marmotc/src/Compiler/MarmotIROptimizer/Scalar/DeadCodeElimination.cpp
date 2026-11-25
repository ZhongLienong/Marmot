#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"
#include "Support/Optional.h"

#include <ranges>
#include <utility>

namespace
{
	// A parameter every edge passes the same value to, or itself, is that
	// value. It dominates each edge, so it dominates the block.
	bool RemoveRedundantParameters(MarmotIRFunction& function)
	{
		const std::vector<std::vector<MarmotIREdge>> incoming = MarmotIRAnalysis::IncomingEdges(function);
		std::vector<MarmotCompiler::Optional<MarmotIRValueId>> replacements(function.m_values.size());
		bool changed = false;
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			std::vector<MarmotIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = parameters.size(); index-- > 0u;)
			{
				const MarmotIRValueId parameter = parameters[index];
				MarmotCompiler::Optional<MarmotIRValueId> only;
				bool is_redundant = true;
				for (const MarmotIREdge& edge : incoming[block])
				{
					const MarmotIRValueId argument = MarmotIRAnalysis::SuccessorOf(function, edge).m_arguments[index];
					if (argument == parameter || argument == only)
					{
						continue;
					}
					if (only.HasValue())
					{
						is_redundant = false;
						break;
					}
					only = argument;
				}
				if (!is_redundant || !only.HasValue())
				{
					continue;
				}
				replacements[parameter.m_index] = only;
				parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
				for (const MarmotIREdge& edge : incoming[block])
				{
					std::vector<MarmotIRValueId>& arguments = MarmotIRAnalysis::SuccessorOf(function, edge).m_arguments;
					arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				}
				changed = true;
			}
		}
		if (changed)
		{
			MarmotIRAnalysis::ReplaceUses(function, replacements);
		}
		return changed;
	}

	// A value is live when an instruction that must run reads it, or a live
	// instruction or live parameter needs it; a parameter's arguments are
	// live when it is.
	bool RemoveDeadValues(MarmotIRFunction& function)
	{
		const std::vector<MarmotCompiler::Optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
		const std::vector<std::vector<MarmotIREdge>> incoming = MarmotIRAnalysis::IncomingEdges(function);
		std::vector<MarmotCompiler::Optional<std::pair<uint32_t, size_t>>> parameter_of(function.m_values.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MarmotIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = 0u; index < parameters.size(); index += 1u)
			{
				parameter_of[parameters[index].m_index] = std::pair{ block, index };
			}
		}

		std::vector<bool> removable(function.m_values.size(), false);
		std::vector<bool> live(function.m_values.size(), false);
		std::vector<MarmotIRValueId> pending;
		const auto mark = [&](MarmotIRValueId value)
		{
			if (!live[value.m_index])
			{
				live[value.m_index] = true;
				pending.push_back(value);
			}
		};
		for (const MarmotIRBlock& block : function.m_blocks)
		{
			for (const MarmotIRInstruction& instruction : block.m_instructions)
			{
				// Compaction invalidates definition positions, so decide whether
				// an instruction can fail before moving any of them.
				const bool is_removable = MarmotIRAnalysis::IsRemovable(function, sites, instruction);
				if (instruction.m_result.HasValue())
				{
					removable[instruction.m_result->m_index] = is_removable;
				}
				if (IsMarmotIRTerminator(instruction.m_op) || !is_removable)
				{
					std::ranges::for_each(instruction.m_operands, mark);
				}
			}
		}
		while (!pending.empty())
		{
			const MarmotIRValueId value = pending.back();
			pending.pop_back();
			const MarmotIRInstruction* definition = MarmotIRAnalysis::Definition(function, sites, value);
			if (definition != nullptr)
			{
				std::ranges::for_each(definition->m_operands, mark);
			}
			else if (parameter_of[value.m_index].HasValue())
			{
				const auto [block, index] = parameter_of[value.m_index].Value();
				for (const MarmotIREdge& edge : incoming[block])
				{
					mark(MarmotIRAnalysis::SuccessorOf(function, edge).m_arguments[index]);
				}
			}
		}

		bool changed = false;
		for (MarmotIRBlock& block : function.m_blocks)
		{
			const size_t before = block.m_instructions.size();
			std::erase_if(block.m_instructions, [&](const MarmotIRInstruction& instruction)
			{
				return instruction.m_result.HasValue() && !live[instruction.m_result->m_index] && removable[instruction.m_result->m_index];
			});
			changed = changed || block.m_instructions.size() != before;
		}
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			std::vector<MarmotIRValueId>& parameters = function.m_blocks[block].m_parameters;
			for (size_t index = parameters.size(); index-- > 0u;)
			{
				if (live[parameters[index].m_index])
				{
					continue;
				}
				parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
				for (const MarmotIREdge& edge : incoming[block])
				{
					std::vector<MarmotIRValueId>& arguments = MarmotIRAnalysis::SuccessorOf(function, edge).m_arguments;
					arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				}
				changed = true;
			}
		}
		return changed;
	}

	MarmotCompiler::Optional<bool> ConstantCondition(const MarmotIRFunction& function, const std::vector<MarmotCompiler::Optional<MarmotIRSite>>& sites, MarmotIRValueId condition)
	{
		const MarmotIRInstruction* definition = MarmotIRAnalysis::Definition(function, sites, condition);
		if (definition == nullptr || definition->m_op != MarmotIROp::Const)
		{
			return MarmotCompiler::None;
		}
		return std::get<bool>(definition->m_immediate);
	}

	bool IsOnlyUnreachable(const MarmotIRFunction& function, const MarmotIRSuccessor& successor)
	{
		const std::vector<MarmotIRInstruction>& instructions = function.Block(successor.m_block).m_instructions;
		return instructions.size() == 1u && instructions.front().m_op == MarmotIROp::Unreachable;
	}

	// The edge a branch always takes: the one its constant condition picks,
	// or the other when one edge reaches only `unreachable`, as an exhaustive
	// match's last test does.
	MarmotCompiler::Optional<bool> TakenEdge(const MarmotIRFunction& function, const std::vector<MarmotCompiler::Optional<MarmotIRSite>>& sites, const MarmotIRInstruction& branch)
	{
		const MarmotCompiler::Optional<bool> condition = ConstantCondition(function, sites, branch.m_operands.front());
		if (condition.HasValue())
		{
			return condition;
		}
		if (IsOnlyUnreachable(function, branch.m_successors[1u]))
		{
			return true;
		}
		if (IsOnlyUnreachable(function, branch.m_successors[0u]))
		{
			return false;
		}
		return MarmotCompiler::None;
	}

	// A branch that always takes one edge, or whose two edges are one, is a
	// jump.
	bool FoldBranches(MarmotIRFunction& function)
	{
		const std::vector<MarmotCompiler::Optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
		bool changed = false;
		for (MarmotIRBlock& block : function.m_blocks)
		{
			MarmotIRInstruction& terminator = block.m_instructions.back();
			if (terminator.m_op != MarmotIROp::Branch)
			{
				continue;
			}
			const MarmotCompiler::Optional<bool> condition = TakenEdge(function, sites, terminator);
			if (!condition.HasValue() && terminator.m_successors[0u] != terminator.m_successors[1u])
			{
				continue;
			}
			terminator = MarmotIRAnalysis::Jump(std::move(terminator.m_successors[condition.ValueOr(true) ? 0u : 1u]), terminator.m_line);
			changed = true;
		}
		return changed;
	}

	// An edge into a block that only jumps on goes where that block goes,
	// passing what it would have: the block's parameters are the edge's
	// arguments, and any other value dominates the block, so it dominates
	// every edge into it. Not when a block it dominates uses its parameters
	// directly, which would then have no definition on the way there.
	bool SkipForwardingBlocks(MarmotIRFunction& function)
	{
		std::vector<uint32_t> uses = MarmotIRAnalysis::CountUses(function);
		bool changed = false;
		for (uint32_t block = 1u; block < function.m_blocks.size(); block += 1u)
		{
			const MarmotIRBlock& forwarding = function.m_blocks[block];
			if (forwarding.m_instructions.size() != 1u || forwarding.m_instructions.front().m_op != MarmotIROp::Jump)
			{
				continue;
			}
			const std::vector<MarmotIRValueId>& passed = forwarding.m_instructions.front().m_successors.front().m_arguments;
			const bool uses_only_to_pass_on = std::ranges::all_of(forwarding.m_parameters, [&](MarmotIRValueId parameter)
			{
				return uses[parameter.m_index] == static_cast<uint32_t>(std::ranges::count(passed, parameter));
			});
			if (!uses_only_to_pass_on)
			{
				continue;
			}
			const MarmotIRSuccessor& onward = forwarding.m_instructions.front().m_successors.front();
			if (onward.m_block.m_index == block)
			{
				continue;
			}
			for (MarmotIRBlock& predecessor : function.m_blocks)
			{
				for (MarmotIRSuccessor& successor : predecessor.m_instructions.back().m_successors)
				{
					if (successor.m_block.m_index != block || &predecessor == &forwarding)
					{
						continue;
					}
					std::vector<MarmotIRValueId> arguments = onward.m_arguments
						| std::views::transform
						(
							[&](MarmotIRValueId argument)
							{
								const std::vector<MarmotIRValueId>::const_iterator parameter = std::ranges::find(forwarding.m_parameters, argument);
								return parameter == forwarding.m_parameters.end() ? argument : successor.m_arguments[static_cast<size_t>(std::distance(forwarding.m_parameters.begin(), parameter))];
							}
						)
						| std::ranges::to<std::vector>();
					successor = MarmotIRSuccessor(onward.m_block, std::move(arguments));
					changed = true;
				}
			}
			uses = MarmotIRAnalysis::CountUses(function);
		}
		return changed;
	}

	// A block whose only predecessor jumps to it joins that predecessor.
	bool MergeBlocks(MarmotIRFunction& function)
	{
		const MarmotIRControlFlow control_flow(function);
		std::vector<size_t> predecessor_counts(function.m_blocks.size(), 0u);
		for (const MarmotIRBlock& block : function.m_blocks)
		{
			for (const MarmotIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				predecessor_counts[successor.m_block.m_index] += 1u;
			}
		}

		std::vector<MarmotCompiler::Optional<MarmotIRValueId>> replacements(function.m_values.size());
		bool changed = false;
		for (const uint32_t block : control_flow.ReversePostorder())
		{
			while (true)
			{
				MarmotIRBlock& predecessor = function.m_blocks[block];
				const MarmotIRInstruction& jump = predecessor.m_instructions.back();
				if (jump.m_op != MarmotIROp::Jump)
				{
					break;
				}
				const uint32_t target = jump.m_successors.front().m_block.m_index;
				if (target == block || target == MarmotIRFunction::s_entry_block.m_index || predecessor_counts[target] != 1u)
				{
					break;
				}
				MarmotIRBlock& merged = function.m_blocks[target];
				for (size_t index = 0u; index < merged.m_parameters.size(); index += 1u)
				{
					replacements[merged.m_parameters[index].m_index] = jump.m_successors.front().m_arguments[index];
				}
				merged.m_parameters.clear();
				predecessor.m_instructions.pop_back();
				std::ranges::move(merged.m_instructions, std::back_inserter(predecessor.m_instructions));
				// What is left does nothing and nothing reaches it.
				merged.m_instructions.clear();
				merged.m_instructions.push_back(MarmotIRAnalysis::Unreachable(0));
				predecessor_counts[target] = 0u;
				changed = true;
			}
		}
		if (changed)
		{
			MarmotIRAnalysis::ReplaceUses(function, replacements);
		}
		return changed;
	}

	bool EliminateDeadCode(MarmotIRFunction& function)
	{
		const size_t blocks_before = function.m_blocks.size();
		bool changed_any = false;
		// Each step only removes, so the rounds end; the bound is for a cycle
		// of blocks that only jump to each other, which never settles.
		for (size_t round = 0u; round <= function.m_blocks.size(); round += 1u)
		{
			MarmotIRAnalysis::RemoveUnreachableBlocks(function);
			bool changed = FoldBranches(function);
			changed = RemoveRedundantParameters(function) || changed;
			changed = RemoveDeadValues(function) || changed;
			changed = SkipForwardingBlocks(function) || changed;
			MarmotIRAnalysis::RemoveUnreachableBlocks(function);
			changed = MergeBlocks(function) || changed;
			changed_any = changed_any || changed;
			if (!changed)
			{
				break;
			}
		}
		MarmotIRAnalysis::RemoveUnreachableBlocks(function);
		return changed_any || function.m_blocks.size() != blocks_before;
	}
}

bool RunDeadCodeElimination(MarmotIRModule& module)
{
	return MarmotIRAnalysis::TransformFunctions(module, EliminateDeadCode);
}
