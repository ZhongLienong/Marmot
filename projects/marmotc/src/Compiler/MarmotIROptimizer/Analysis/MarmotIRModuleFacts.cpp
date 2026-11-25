#include "MarmotIRModuleFacts.h"
#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Support/Optional.h"

#include <algorithm>
#include <ranges>

MarmotIRModuleFacts::MarmotIRModuleFacts(const MarmotIRModule& module)
	: m_global_functions(module.m_globals.size()),
	m_self_captures(module.m_functions.size()),
	m_binds_only_known_closures(true)
{
	std::vector<uint32_t> closures_made(module.m_functions.size(), 0u);
	// Per function, whether each capture was bound to its own closure at every
	// site that made one so far.
	std::vector<std::vector<bool>> bound_to_self(module.m_functions.size());
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		bound_to_self[function].assign(module.m_functions[function].m_capture_types.size(), true);
	}

	for (const MarmotIRFunction& maker : module.m_functions)
	{
		const std::vector<MarmotCompiler::Optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(maker);
		for (const MarmotIRBlock& block : maker.m_blocks)
		{
			for (const MarmotIRInstruction& instruction : block.m_instructions)
			{
				if (instruction.m_op == MarmotIROp::MakeClosure)
				{
					const uint32_t made = std::get<MarmotIRFunctionId>(instruction.m_immediate).m_index;
					closures_made[made] += 1u;
					std::vector<bool>& captures = bound_to_self[made];
					const std::vector<bool> bound = std::views::zip(std::views::iota(0uz), captures)
						| std::views::transform
						(
							[&](const auto& capture)
							{
								const auto& [index, was_self] = capture;
								const bool is_passed = index < instruction.m_operands.size();
								const bool binds_self = std::ranges::any_of(block.m_instructions, [&](const MarmotIRInstruction& bind)
								{
									return bind.m_op == MarmotIROp::BindCaptures && bind.m_operands[0u] == instruction.m_result && bind.m_operands[1u] == instruction.m_result && std::get<MarmotIRIndex>(bind.m_immediate).m_value == static_cast<uint32_t>(index);
								});
								return was_self && !is_passed && binds_self;
							}
						)
						| std::ranges::to<std::vector>();
					captures = bound;
				}
				else if (instruction.m_op == MarmotIROp::BindCaptures)
				{
					const MarmotIRInstruction* closure = MarmotIRAnalysis::Definition(maker, sites, instruction.m_operands[0u]);
					m_binds_only_known_closures = m_binds_only_known_closures && closure != nullptr && closure->m_op == MarmotIROp::MakeClosure;
				}
				else if (instruction.m_op == MarmotIROp::GlobalDefine && module.m_top_level.HasValue() && &maker == &module.Function(module.m_top_level.Value()))
				{
					const MarmotIRInstruction* stored = MarmotIRAnalysis::Definition(maker, sites, instruction.m_operands[0u]);
					if (stored != nullptr && stored->m_op == MarmotIROp::MakeClosure && stored->m_operands.empty())
					{
						const MarmotIRFunctionId function = std::get<MarmotIRFunctionId>(stored->m_immediate);
						if (module.Function(function).m_capture_types.empty())
						{
							m_global_functions[std::get<MarmotIRGlobalSlot>(instruction.m_immediate).m_value] = function;
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
			| std::views::transform([&](bool bound) { return bound && is_made && m_binds_only_known_closures; })
			| std::ranges::to<std::vector>();
	}
}

MarmotCompiler::Optional<MarmotIRFunctionId> MarmotIRModuleFacts::GlobalFunction(MarmotIRGlobalSlot slot) const
{
	return m_global_functions[slot.m_value];
}

bool MarmotIRModuleFacts::IsSelfCapture(MarmotIRFunctionId function, uint32_t capture) const
{
	return m_self_captures[function.m_index][capture];
}

bool MarmotIRModuleFacts::BindsOnlyKnownClosures() const
{
	return m_binds_only_known_closures;
}
