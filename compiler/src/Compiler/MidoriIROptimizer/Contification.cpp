#include "Compiler/MidoriIROptimizer/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRBodyCopy.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// The one function that names `function` other than itself, when every
	// such name is a tail call.
	std::optional<uint32_t> OnlyTailCaller(const MidoriIRModule& module, uint32_t function)
	{
		std::optional<uint32_t> caller;
		for (uint32_t other = 0u; other < module.m_functions.size(); other += 1u)
		{
			for (const MidoriIRBlock& block : module.m_functions[other].m_blocks)
			{
				for (const MidoriIRInstruction& instruction : block.m_instructions)
				{
					const bool names_it = std::holds_alternative<MidoriIRFunctionId>(instruction.m_immediate) && std::get<MidoriIRFunctionId>(instruction.m_immediate).m_index == function;
					if (!names_it)
					{
						continue;
					}
					if (other == function && instruction.m_op != MidoriIROp::MakeClosure)
					{
						continue;
					}
					if (instruction.m_op != MidoriIROp::TailCall || (caller.has_value() && caller != other))
					{
						return std::nullopt;
					}
					caller = other;
				}
			}
		}
		return caller;
	}

	void Contify(MidoriIRModule& module, uint32_t function, uint32_t caller_index)
	{
		const MidoriIRFunction& callee = module.m_functions[function];
		MidoriIRFunction& caller = module.m_functions[caller_index];
		const MidoriIRCopiedBody body = MidoriIRBodyCopy::Copy(callee, caller, static_cast<uint32_t>(caller.m_blocks.size()), std::nullopt);
		for (uint32_t block = 0u; block < body.m_first; block += 1u)
		{
			MidoriIRInstruction& terminator = caller.m_blocks[block].m_instructions.back();
			const bool calls_it = terminator.m_op == MidoriIROp::TailCall && std::holds_alternative<MidoriIRFunctionId>(terminator.m_immediate) && std::get<MidoriIRFunctionId>(terminator.m_immediate).m_index == function;
			if (calls_it)
			{
				terminator = MidoriIRAnalysis::Jump(MidoriIRSuccessor(body.Entry(), terminator.m_operands), terminator.m_line);
			}
		}
	}
}

std::string_view ContificationPass::Name() const
{
	return "Contification";
}

void ContificationPass::Run(MidoriIRModule& module) const
{
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		if (module.m_top_level.has_value() && module.m_top_level->m_index == function)
		{
			continue;
		}
		const MidoriIRFunction& callee = module.m_functions[function];
		// Its body runs in the caller's frame, which a stack trace would name.
		if (!callee.m_capture_types.empty() || !MidoriIRAnalysis::IsFrameTransparent(callee))
		{
			continue;
		}
		const std::optional<uint32_t> caller = OnlyTailCaller(module, function);
		if (caller.has_value() && module.m_functions[caller.value()].m_source_module == callee.m_source_module)
		{
			Contify(module, function, caller.value());
		}
	}
}
