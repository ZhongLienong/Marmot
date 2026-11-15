#include "MarmotIROptimizer.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <utility>
#include "Utility/Diagnostics/Diagnostics.h"

#if MARMOT_ENABLE_OPTIMIZER_STATS
#include <format>
#include <iterator>
#include <numeric>
#endif

namespace
{
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

MarmotIRPassFailure::MarmotIRPassFailure(std::string pass, std::vector<MarmotIRViolation> violations)
	: m_pass(std::move(pass)),
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

std::expected<void, MarmotIRPassFailure> MarmotIROptimizer::Optimize(MarmotIRModule& module)
{
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
				if (!VerifiesEachPass())
				{
					continue;
				}
				std::vector<MarmotIRViolation> violations = MarmotIRVerifier(module).Verify();
				if (!violations.empty())
				{
					return std::unexpected(MarmotIRPassFailure(std::string(pass.m_name), std::move(violations)));
				}
			}
			if (!changed)
			{
				break;
			}
		}
	}
	return {};
}

bool MarmotIROptimizer::VerifiesEachPass()
{
	return MARMOT_DEBUG_INFO;
}

#if MARMOT_ENABLE_OPTIMIZER_STATS
const std::string& MarmotIROptimizer::Log() const
{
	return m_log;
}
#endif
