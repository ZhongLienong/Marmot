#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// One rewrite of a module, reporting whether it changed the IR. A pass that
// leaves invalid IR is a compiler bug the verifier catches.
class MidoriIRPass
{
public:
	virtual ~MidoriIRPass() = default;

	virtual std::string_view Name() const = 0;
	virtual bool Run(MidoriIRModule& module) const = 0;
};

// The pass that left the module invalid, and what the verifier found.
struct MidoriIRPassFailure
{
	std::string m_pass;
	std::vector<MidoriIRViolation> m_violations;

	MidoriIRPassFailure(std::string pass, std::vector<MidoriIRViolation> violations);
};

// Repeats transformations that expose opportunities for one another, stopping
// when unchanged or at the round budget. A supplied pass list runs once. In
// Dev and Debug builds the verifier runs after every pass.
class MidoriIROptimizer
{
private:
	struct PassGroup
	{
		std::vector<std::unique_ptr<MidoriIRPass>> m_passes;
		size_t m_max_rounds;

		PassGroup(std::vector<std::unique_ptr<MidoriIRPass>> passes, size_t max_rounds);
	};

	std::vector<PassGroup> m_groups;
#if MIDORI_ENABLE_OPTIMIZER_STATS
	std::string m_log;
#endif

public:
	MidoriIROptimizer();
	explicit MidoriIROptimizer(std::vector<std::unique_ptr<MidoriIRPass>> passes);

	std::expected<void, MidoriIRPassFailure> Optimize(MidoriIRModule& module);

	static bool VerifiesEachPass();

#if MIDORI_ENABLE_OPTIMIZER_STATS
	// One line for each pass that changed the module, with instruction counts.
	const std::string& Log() const;
#endif
};
