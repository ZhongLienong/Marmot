#include "MidoriIROptimizer.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <utility>

namespace
{
	template<typename... Passes>
	std::vector<std::unique_ptr<MidoriIRPass>> MakePasses()
	{
		std::vector<std::unique_ptr<MidoriIRPass>> passes;
		(passes.push_back(std::make_unique<Passes>()), ...);
		return passes;
	}
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

std::expected<void, MidoriIRPassFailure> MidoriIROptimizer::Optimize(MidoriIRModule& module) const
{
	for (const std::unique_ptr<MidoriIRPass>& pass : m_passes)
	{
		pass->Run(module);
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
