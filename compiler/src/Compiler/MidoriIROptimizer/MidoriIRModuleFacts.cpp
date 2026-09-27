#include "MidoriIRModuleFacts.h"
#include "Compiler/MidoriIROptimizer/MidoriIRAnalysis.h"

#include <algorithm>
#include <ranges>

MidoriIRModuleFacts::MidoriIRModuleFacts(const MidoriIRModule& module)
	: m_global_functions(module.m_globals.size()),
	m_self_captures(module.m_functions.size())
{
	std::vector<uint32_t> closures_made(module.m_functions.size(), 0u);
	// Per function, whether each capture was bound to its own closure at every
	// site that made one so far.
	std::vector<std::vector<bool>> bound_to_self(module.m_functions.size());
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		bound_to_self[function].assign(module.m_functions[function].m_capture_types.size(), true);
	}

	bool binds_unknown_closure = false;
	for (const MidoriIRFunction& maker : module.m_functions)
	{
		const std::vector<std::optional<MidoriIRSite>> sites = MidoriIRAnalysis::DefinitionSites(maker);
		for (const MidoriIRBlock& block : maker.m_blocks)
		{
			for (const MidoriIRInstruction& instruction : block.m_instructions)
			{
				if (instruction.m_op == MidoriIROp::MakeClosure)
				{
					const uint32_t made = std::get<MidoriIRFunctionId>(instruction.m_immediate).m_index;
					closures_made[made] += 1u;
					std::vector<bool>& captures = bound_to_self[made];
					const std::vector<bool> bound = captures
						| std::views::enumerate
						| std::views::transform([&](const auto& capture)
						{
							const auto& [index, was_self] = capture;
							const bool is_passed = static_cast<size_t>(index) < instruction.m_operands.size();
							const bool binds_self = std::ranges::any_of(block.m_instructions, [&](const MidoriIRInstruction& bind)
							{
								return bind.m_op == MidoriIROp::BindCaptures && bind.m_operands[0u] == instruction.m_result && bind.m_operands[1u] == instruction.m_result && std::get<MidoriIRIndex>(bind.m_immediate).m_value == static_cast<uint32_t>(index);
							});
							return was_self && !is_passed && binds_self;
						})
						| std::ranges::to<std::vector>();
					captures = bound;
				}
				else if (instruction.m_op == MidoriIROp::BindCaptures)
				{
					const MidoriIRInstruction* closure = MidoriIRAnalysis::Definition(maker, sites, instruction.m_operands[0u]);
					binds_unknown_closure = binds_unknown_closure || closure == nullptr || closure->m_op != MidoriIROp::MakeClosure;
				}
				else if (instruction.m_op == MidoriIROp::GlobalDefine && module.m_top_level.has_value() && &maker == &module.Function(module.m_top_level.value()))
				{
					const MidoriIRInstruction* stored = MidoriIRAnalysis::Definition(maker, sites, instruction.m_operands[0u]);
					if (stored != nullptr && stored->m_op == MidoriIROp::MakeClosure && stored->m_operands.empty())
					{
						const MidoriIRFunctionId function = std::get<MidoriIRFunctionId>(stored->m_immediate);
						if (module.Function(function).m_capture_types.empty())
						{
							m_global_functions[std::get<MidoriIRGlobalSlot>(instruction.m_immediate).m_value] = function;
						}
					}
				}
			}
		}
	}

	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		const bool is_made = closures_made[function] != 0u;
		m_self_captures[function] = bound_to_self[function]
			| std::views::transform([&](bool bound) { return bound && is_made && !binds_unknown_closure; })
			| std::ranges::to<std::vector>();
	}
}

std::optional<MidoriIRFunctionId> MidoriIRModuleFacts::GlobalFunction(MidoriIRGlobalSlot slot) const
{
	return m_global_functions[slot.m_value];
}

bool MidoriIRModuleFacts::IsSelfCapture(MidoriIRFunctionId function, uint32_t capture) const
{
	return m_self_captures[function.m_index][capture];
}
