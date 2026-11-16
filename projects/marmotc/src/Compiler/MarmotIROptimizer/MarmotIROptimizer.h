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

// What produced invalid IR: lowering, a named pass, or the optimizer's final
// output. The caller adds source context when rendering the violations.
struct MarmotIROptimizationFailure
{
	std::string m_producer;
	std::vector<MarmotIRViolation> m_violations;

	MarmotIROptimizationFailure(std::string producer, std::vector<MarmotIRViolation> violations);
};

// Repeats transformations that expose opportunities for one another, stopping
// when unchanged or at the round budget. A supplied pass list runs once.
// Dev and Debug builds verify the input and every pass; Release verifies the
// final output. Success guarantees valid IR with either pass schedule.
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

	std::expected<void, MarmotIROptimizationFailure> Optimize(MarmotIRModule& module);

#if MARMOT_ENABLE_OPTIMIZER_STATS
	// One line for each pass that changed the module, with instruction counts.
	const std::string& Log() const;
#endif
};
