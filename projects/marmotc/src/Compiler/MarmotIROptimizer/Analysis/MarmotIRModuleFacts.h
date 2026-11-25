#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"
#include "Support/Optional.h"

#include <cstdint>
#include <vector>

// What a pass may know about a module's functions from how the whole module
// uses them. It is computed once, before the pass changes anything.
class MarmotIRModuleFacts
{
private:
	std::vector<MarmotCompiler::Optional<MarmotIRFunctionId>> m_global_functions;
	std::vector<std::vector<bool>> m_self_captures;
	bool m_binds_only_known_closures;

public:
	explicit MarmotIRModuleFacts(const MarmotIRModule& module);

	// The function a global holds, when the top-level function defines it as
	// that function with nothing captured. A global is never set again.
	MarmotCompiler::Optional<MarmotIRFunctionId> GlobalFunction(MarmotIRGlobalSlot slot) const;

	// Whether every closure of `function` the module makes has this capture
	// bound to the closure itself, as a local function that names itself does.
	bool IsSelfCapture(MarmotIRFunctionId function, uint32_t capture) const;

	// Whether every BindCaptures fills a closure its own function makes.
	bool BindsOnlyKnownClosures() const;
};
