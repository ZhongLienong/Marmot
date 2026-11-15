#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// A loop: its header, and every block from which a back edge to the
	// header is reached without passing through it.
	struct Loop
	{
		uint32_t m_header;
		std::vector<bool> m_blocks;
		size_t m_size;
	};

	std::vector<Loop> FindLoops(const MarmotIRFunction& function, const MarmotIRControlFlow& control_flow)
	{
		std::vector<Loop> loops;
		for (const uint32_t header : control_flow.ReversePostorder())
		{
			std::vector<uint32_t> pending;
			for (const uint32_t predecessor : control_flow.Predecessors(header))
			{
				if (control_flow.Dominates(header, predecessor))
				{
					pending.push_back(predecessor);
				}
			}
			if (pending.empty())
			{
				continue;
			}
			Loop loop{ header, std::vector<bool>(function.m_blocks.size(), false), 1u };
			loop.m_blocks[header] = true;
			while (!pending.empty())
			{
				const uint32_t block = pending.back();
				pending.pop_back();
				if (loop.m_blocks[block])
				{
					continue;
				}
				loop.m_blocks[block] = true;
				loop.m_size += 1u;
				std::ranges::copy(control_flow.Predecessors(block), std::back_inserter(pending));
			}
			loops.push_back(std::move(loop));
		}
		// Inner loops first: what leaves one may leave the next one out too.
		std::ranges::sort(loops, {}, &Loop::m_size);
		return loops;
	}

	// Moving an instruction out of a loop runs it even when the loop body
	// would not have, so it must neither fail nor allocate; and it runs once,
	// so it must give the same value every time.
	bool IsHoistable(const MarmotIRInstruction& instruction)
	{
		if (!instruction.m_result.has_value() || IsMarmotIRTerminator(instruction.m_op))
		{
			return false;
		}
		switch (instruction.m_effect.m_kind)
		{
		case MarmotIREffectKind::Pure:
			return true;
		case MarmotIREffectKind::Read:
			return MarmotIRAnalysis::IsStableRead(instruction);
		case MarmotIREffectKind::Fault:
		case MarmotIREffectKind::Alloc:
		case MarmotIREffectKind::Io:
		case MarmotIREffectKind::Call:
			return false;
		}
		std::unreachable();
	}

	// The one block outside the loop that enters it, when it only jumps to
	// the header.
	std::optional<uint32_t> Preheader(const MarmotIRFunction& function, const MarmotIRControlFlow& control_flow, const Loop& loop)
	{
		const std::vector<uint32_t> outside = control_flow.Predecessors(loop.m_header)
			| std::views::filter([&loop](uint32_t predecessor) { return !loop.m_blocks[predecessor]; })
			| std::ranges::to<std::vector>();
		if (outside.size() != 1u || function.m_blocks[outside.front()].m_instructions.back().m_op != MarmotIROp::Jump)
		{
			return std::nullopt;
		}
		return outside.front();
	}

	// Hoisting moves instructions but no edge, so one graph serves every loop.
	bool Hoist(MarmotIRFunction& function, const MarmotIRControlFlow& control_flow, const Loop& loop)
	{
		const std::optional<uint32_t> preheader = Preheader(function, control_flow, loop);
		if (!preheader.has_value())
		{
			return false;
		}
		std::vector<bool> defined_inside(function.m_values.size(), false);
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			if (!loop.m_blocks[block])
			{
				continue;
			}
			for (const MarmotIRValueId parameter : function.m_blocks[block].m_parameters)
			{
				defined_inside[parameter.m_index] = true;
			}
			for (const MarmotIRInstruction& instruction : function.m_blocks[block].m_instructions)
			{
				if (instruction.m_result.has_value())
				{
					defined_inside[instruction.m_result->m_index] = true;
				}
			}
		}

		std::vector<MarmotIRInstruction> hoisted;
		for (const uint32_t block : control_flow.ReversePostorder())
		{
			if (!loop.m_blocks[block])
			{
				continue;
			}
			std::vector<MarmotIRInstruction>& instructions = function.m_blocks[block].m_instructions;
			std::erase_if(instructions, [&](const MarmotIRInstruction& instruction)
			{
				const bool is_invariant = IsHoistable(instruction) && std::ranges::none_of(instruction.m_operands, [&](MarmotIRValueId operand) { return defined_inside[operand.m_index]; });
				if (!is_invariant)
				{
					return false;
				}
				defined_inside[instruction.m_result->m_index] = false;
				hoisted.push_back(instruction);
				return true;
			});
		}
		std::vector<MarmotIRInstruction>& target = function.m_blocks[preheader.value()].m_instructions;
		target.insert(target.end() - 1, std::make_move_iterator(hoisted.begin()), std::make_move_iterator(hoisted.end()));
		return !hoisted.empty();
	}

	bool HoistInvariants(MarmotIRFunction& function)
	{
		const MarmotIRControlFlow control_flow(function);
		bool changed = false;
		for (const Loop& loop : FindLoops(function, control_flow))
		{
			changed = Hoist(function, control_flow, loop) || changed;
		}
		return changed;
	}
}

bool RunLoopInvariantCodeMotion(MarmotIRModule& module)
{
	return MarmotIRAnalysis::TransformFunctions(module, HoistInvariants);
}
