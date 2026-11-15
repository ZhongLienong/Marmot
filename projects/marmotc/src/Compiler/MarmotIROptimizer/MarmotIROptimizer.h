#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

// One rewrite of a module, reporting whether it changed the IR. A pass that
// leaves invalid IR is a compiler bug the verifier catches.
struct MarmotIRPass
{
	std::string_view m_name;
	bool (*m_run)(MarmotIRModule& module);
};

// The pass that left the module invalid, and what the verifier found.
struct MarmotIRPassFailure
{
	std::string m_pass;
	std::vector<MarmotIRViolation> m_violations;

	MarmotIRPassFailure(std::string pass, std::vector<MarmotIRViolation> violations);
};

// Repeats transformations that expose opportunities for one another, stopping
// when unchanged or at the round budget. A supplied pass list runs once. In
// Dev and Debug builds the verifier runs after every pass.
class MarmotIROptimizer
{
private:
	struct PassGroup
	{
		std::vector<MarmotIRPass> m_passes;
		size_t m_max_rounds;

		PassGroup(std::vector<MarmotIRPass> passes, size_t max_rounds);
	};

	std::vector<PassGroup> m_groups;
#if MARMOT_ENABLE_OPTIMIZER_STATS
	std::string m_log;
#endif

public:
	MarmotIROptimizer();
	explicit MarmotIROptimizer(std::vector<MarmotIRPass> passes);

	std::expected<void, MarmotIRPassFailure> Optimize(MarmotIRModule& module);

	static bool VerifiesEachPass();

#if MARMOT_ENABLE_OPTIMIZER_STATS
	// One line for each pass that changed the module, with instruction counts.
	const std::string& Log() const;
#endif
};
