#include "MidoriIROptimizer.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <utility>

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

// Each pass sets up the next: the graph is cleaned before calls are looked at;
// a self tail call is a loop and a known closure a direct call before bodies
// are copied; copying exposes values made only to be taken apart and
// constants; the cleanup runs last. Self tail calls are looked for again after
// inlining, which can make a mutual tail call a self one.
MidoriIROptimizer::MidoriIROptimizer()
	: m_passes(MakePasses
	<
		DeadCodeEliminationPass,
		SelfTailCallPass,
		ClosureConversionPass,
		ContificationPass,
		InliningPass,
		SelfTailCallPass,
		ScalarReplacementPass,
		SccpPass,
		StrengthReductionPass,
		GlobalValueNumberingPass,
		LoopInvariantCodeMotionPass,
		DeadCodeEliminationPass
	>())
{
}

MidoriIROptimizer::MidoriIROptimizer(std::vector<std::unique_ptr<MidoriIRPass>> passes)
	: m_passes(std::move(passes))
{
}

std::expected<void, MidoriIRPassFailure> MidoriIROptimizer::Optimize(MidoriIRModule& module)
{
	for (const std::unique_ptr<MidoriIRPass>& pass : m_passes)
	{
#if MIDORI_ENABLE_OPTIMIZER_STATS
		const size_t before = InstructionCount(module);
		pass->Run(module);
		const size_t after = InstructionCount(module);
		if (after != before)
		{
			std::format_to(std::back_inserter(m_log), "  {}: {} -> {} instructions\n", pass->Name(), before, after);
		}
#else
		pass->Run(module);
#endif
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
