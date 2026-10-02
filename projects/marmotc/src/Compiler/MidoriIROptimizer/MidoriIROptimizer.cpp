#include "MidoriIROptimizer.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <utility>
#include "Utility/Diagnostics/Diagnostics.h"

#if MIDORI_ENABLE_OPTIMIZER_STATS
#include <format>
#include <iterator>
#include <numeric>
#endif

namespace
{
	template<typename... Passes>
	std::vector<std::unique_ptr<MidoriIRPass>> MakePasses()
	{
		std::vector<std::unique_ptr<MidoriIRPass>> passes;
		(passes.push_back(std::make_unique<Passes>()), ...);
		return passes;
	}

#if MIDORI_ENABLE_OPTIMIZER_STATS
	size_t InstructionCount(const MidoriIRModule& module)
	{
		return std::accumulate(module.m_functions.begin(), module.m_functions.end(), 0uz, [](size_t count, const MidoriIRFunction& function)
		{
			return std::accumulate(function.m_blocks.begin(), function.m_blocks.end(), count, [](size_t block_count, const MidoriIRBlock& block)
			{
				return block_count + block.m_instructions.size();
			});
		});
	}
#endif
}

MidoriIRPassFailure::MidoriIRPassFailure(std::string pass, std::vector<MidoriIRViolation> violations)
	: m_pass(std::move(pass)),
	m_violations(std::move(violations))
{
}

MidoriIROptimizer::PassGroup::PassGroup(std::vector<std::unique_ptr<MidoriIRPass>> passes, size_t max_rounds)
	: m_passes(std::move(passes)),
	m_max_rounds(max_rounds)
{
}

// Inlining exposes known closures; scalar rewrites expose constants and make
// more callees small and frame transparent. Revisit these together, keeping
// the inliner's size limits and a round budget to bound compilation work.
MidoriIROptimizer::MidoriIROptimizer()
{
	m_groups.emplace_back(MakePasses<DeadCodeEliminationPass, SelfTailCallPass>(), 1u);
	m_groups.emplace_back(MakePasses
	<
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
	>(), 8u);
	m_groups.emplace_back(MakePasses<LoopInvariantCodeMotionPass, DeadCodeEliminationPass>(), 1u);
}

MidoriIROptimizer::MidoriIROptimizer(std::vector<std::unique_ptr<MidoriIRPass>> passes)
{
	m_groups.emplace_back(std::move(passes), 1u);
}

std::expected<void, MidoriIRPassFailure> MidoriIROptimizer::Optimize(MidoriIRModule& module)
{
	for (const PassGroup& group : m_groups)
	{
		for (size_t round = 0u; round < group.m_max_rounds; round += 1u)
		{
			bool changed = false;
			for (const std::unique_ptr<MidoriIRPass>& pass : group.m_passes)
			{
#if MIDORI_ENABLE_OPTIMIZER_STATS
				const bool statistics = CompilerDiagnostics::StatisticsEnabled();
				const size_t before = statistics ? InstructionCount(module) : 0uz;
				const bool pass_changed = pass->Run(module);
				if (statistics && pass_changed)
				{
					std::format_to(std::back_inserter(m_log), "  {}: {} -> {} instructions\n", pass->Name(), before, InstructionCount(module));
				}
#else
				const bool pass_changed = pass->Run(module);
#endif
				changed = changed || pass_changed;
				if (!VerifiesEachPass())
				{
					continue;
				}
				std::vector<MidoriIRViolation> violations = MidoriIRVerifier(module).Verify();
				if (!violations.empty())
				{
					return std::unexpected(MidoriIRPassFailure(std::string(pass->Name()), std::move(violations)));
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

bool MidoriIROptimizer::VerifiesEachPass()
{
	return MIDORI_DEBUG_INFO;
}

#if MIDORI_ENABLE_OPTIMIZER_STATS
const std::string& MidoriIROptimizer::Log() const
{
	return m_log;
}
#endif
