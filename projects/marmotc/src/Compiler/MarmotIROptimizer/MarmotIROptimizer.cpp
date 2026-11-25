#include "MarmotIROptimizer.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <format>
#include <utility>
#include "Utility/Diagnostics/Diagnostics.h"
#include "Support/Expected.h"

#if MARMOT_ENABLE_OPTIMIZER_STATS
#include <iterator>
#include <numeric>
#endif

namespace
{
	constexpr bool s_verify_each_pass = MARMOT_DEBUG_INFO;

	MarmotCompiler::Expected<void, MarmotIROptimizationFailure> Verify(const MarmotIRModule& module, std::string_view producer)
	{
		std::vector<MarmotIRViolation> violations = MarmotIRVerifier(module).Verify();
		if (violations.empty())
		{
			return {};
		}
		return MarmotCompiler::Failure(MarmotIROptimizationFailure(std::string(producer), std::move(violations)));
	}

#if MARMOT_ENABLE_OPTIMIZER_STATS
	size_t InstructionCount(const MarmotIRModule& module)
	{
		return std::accumulate(module.m_functions.begin(), module.m_functions.end(), 0uz, [](size_t count, const MarmotIRFunction& function)
		{
			return std::accumulate(function.m_blocks.begin(), function.m_blocks.end(), count, [](size_t block_count, const MarmotIRBlock& block)
			{
				return block_count + block.m_instructions.size();
			});
		});
	}
#endif
}

MarmotIROptimizationFailure::MarmotIROptimizationFailure(std::string producer, std::vector<MarmotIRViolation> violations)
	: m_producer(std::move(producer)),
	m_violations(std::move(violations))
{
}

MarmotIROptimizer::PassGroup::PassGroup(std::vector<MarmotIRPass> passes, size_t max_rounds)
	: m_passes(std::move(passes)),
	m_max_rounds(max_rounds)
{
}

// Inlining exposes known closures; scalar rewrites expose constants and make
// more callees small and frame transparent. Revisit these together, keeping
// the inliner's size limits and a round budget to bound compilation work.
MarmotIROptimizer::MarmotIROptimizer()
{
	m_groups.emplace_back(std::vector<MarmotIRPass>{ DeadCodeEliminationPass, SelfTailCallPass }, 1u);
	m_groups.emplace_back(std::vector<MarmotIRPass>
	{
		ClosureConversionPass,
		ContificationPass,
		InliningPass,
		SelfTailCallPass,
		KnownConstructorThreadingPass,
		DeadCodeEliminationPass,
		ScalarReplacementPass,
		ParameterUnboxingPass,
		SccpPass,
		StrengthReductionPass,
		GlobalValueNumberingPass,
		DeadCodeEliminationPass
	}, 8u);
	m_groups.emplace_back(std::vector<MarmotIRPass>{ LoopInvariantCodeMotionPass, DeadCodeEliminationPass }, 1u);
}

MarmotIROptimizer::MarmotIROptimizer(std::vector<MarmotIRPass> passes)
{
	m_groups.emplace_back(std::move(passes), 1u);
}

MarmotCompiler::Expected<void, MarmotIROptimizationFailure> MarmotIROptimizer::Optimize(MarmotIRModule& module)
{
	if (s_verify_each_pass)
	{
		MarmotCompiler::Expected<void, MarmotIROptimizationFailure> verified = Verify(module, "Lowering");
		if (!verified.HasValue())
		{
			return verified;
		}
	}
	for (const PassGroup& group : m_groups)
	{
		for (size_t round = 0u; round < group.m_max_rounds; round += 1u)
		{
			bool changed = false;
			for (const MarmotIRPass& pass : group.m_passes)
			{
#if MARMOT_ENABLE_OPTIMIZER_STATS
				const bool statistics = CompilerDiagnostics::StatisticsEnabled();
				const size_t before = statistics ? InstructionCount(module) : 0uz;
				const bool pass_changed = pass.m_run(module);
				if (statistics && pass_changed)
				{
					std::format_to(std::back_inserter(m_log), "  {}: {} -> {} instructions\n", pass.m_name, before, InstructionCount(module));
				}
#else
				const bool pass_changed = pass.m_run(module);
#endif
				changed = changed || pass_changed;
				if (!s_verify_each_pass)
				{
					continue;
				}
				std::vector<MarmotIRViolation> violations = MarmotIRVerifier(module).Verify();
				if (!violations.empty())
				{
					return MarmotCompiler::Failure(MarmotIROptimizationFailure(std::format("The MarmotIR pass {}", pass.m_name), std::move(violations)));
				}
			}
			if (!changed)
			{
				break;
			}
		}
	}
	if (!s_verify_each_pass)
	{
		return Verify(module, "The MarmotIR optimizer");
	}
	return {};
}

#if MARMOT_ENABLE_OPTIMIZER_STATS
const std::string& MarmotIROptimizer::Log() const
{
	return m_log;
}
#endif
