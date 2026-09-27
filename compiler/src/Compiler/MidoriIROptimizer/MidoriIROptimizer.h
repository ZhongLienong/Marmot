#pragma once

#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/MidoriIR/MidoriIR.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// One rewrite of a module. A pass reports nothing: the IR it leaves must be
// valid, and a pass that breaks it is a compiler bug the verifier catches.
class MidoriIRPass
{
public:
	virtual ~MidoriIRPass() = default;

	virtual std::string_view Name() const = 0;
	virtual void Run(MidoriIRModule& module) const = 0;
};

// The pass that left the module invalid, and what the verifier found.
struct MidoriIRPassFailure
{
	std::string m_pass;
	std::vector<MidoriIRViolation> m_violations;

	MidoriIRPassFailure(std::string pass, std::vector<MidoriIRViolation> violations);
};

// Runs a fixed list of passes once, in order: SSA makes iterating to a
// fixpoint unnecessary, and a pass that helps a later one runs before it. In
// Development and Debug builds the verifier runs after every pass.
class MidoriIROptimizer
{
private:
	std::vector<std::unique_ptr<MidoriIRPass>> m_passes;
#if MIDORI_ENABLE_OPTIMIZER_STATS
	std::string m_log;
#endif

public:
	MidoriIROptimizer();
	explicit MidoriIROptimizer(std::vector<std::unique_ptr<MidoriIRPass>> passes);

	std::expected<void, MidoriIRPassFailure> Optimize(MidoriIRModule& module);

	static bool VerifiesEachPass();

#if MIDORI_ENABLE_OPTIMIZER_STATS
	// One line for each pass that changed the module's instruction count.
	const std::string& Log() const;
#endif
};
