#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/Interprocedural/MarmotIRBodyCopy.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// The one function that names `function` other than itself, when every
	// such name is a tail call.
	std::optional<uint32_t> OnlyTailCaller(const MarmotIRModule& module, uint32_t function)
	{
		std::optional<uint32_t> caller;
		for (uint32_t other = 0u; other < module.m_functions.size(); other += 1u)
		{
			for (const MarmotIRBlock& block : module.m_functions[other].m_blocks)
			{
				for (const MarmotIRInstruction& instruction : block.m_instructions)
				{
					const bool names_it = std::holds_alternative<MarmotIRFunctionId>(instruction.m_immediate) && std::get<MarmotIRFunctionId>(instruction.m_immediate).m_index == function;
					if (!names_it)
					{
						continue;
					}
					if (other == function && instruction.m_op != MarmotIROp::MakeClosure)
					{
						continue;
					}
					if (instruction.m_op != MarmotIROp::TailCall || (caller.has_value() && caller != other))
					{
						return std::nullopt;
					}
					caller = other;
				}
			}
		}
		return caller;
	}

	void Contify(MarmotIRModule& module, uint32_t function, uint32_t caller_index)
	{
		const MarmotIRFunction& callee = module.m_functions[function];
		MarmotIRFunction& caller = module.m_functions[caller_index];
		const MarmotIRCopiedBody body = MarmotIRBodyCopy::Copy(callee, caller, static_cast<uint32_t>(caller.m_blocks.size()), std::nullopt);
		for (uint32_t block = 0u; block < body.m_first; block += 1u)
		{
			MarmotIRInstruction& terminator = caller.m_blocks[block].m_instructions.back();
			const bool calls_it = terminator.m_op == MarmotIROp::TailCall && std::holds_alternative<MarmotIRFunctionId>(terminator.m_immediate) && std::get<MarmotIRFunctionId>(terminator.m_immediate).m_index == function;
			if (calls_it)
			{
				terminator = MarmotIRAnalysis::Jump(MarmotIRSuccessor(body.Entry(), terminator.m_operands), terminator.m_line);
			}
		}
	}
}

bool RunContification(MarmotIRModule& module)
{
	bool changed = false;
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		if (module.m_top_level.has_value() && module.m_top_level->m_index == function)
		{
			continue;
		}
		const MarmotIRFunction& callee = module.m_functions[function];
		// Its body runs in the caller's frame, which a stack trace would name.
		if (!callee.m_capture_types.empty() || !MarmotIRAnalysis::IsFrameTransparent(callee))
		{
			continue;
		}
		const std::optional<uint32_t> caller = OnlyTailCaller(module, function);
		if (caller.has_value() && module.m_functions[caller.value()].m_source_module == callee.m_source_module)
		{
			Contify(module, function, caller.value());
			changed = true;
		}
	}
	return changed;
}
