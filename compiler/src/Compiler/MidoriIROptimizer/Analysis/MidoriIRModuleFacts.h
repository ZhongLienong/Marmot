#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <cstdint>
#include <optional>
#include <vector>

// What a pass may know about a module's functions from how the whole module
// uses them. It is computed once, before the pass changes anything.
class MidoriIRModuleFacts
{
private:
	std::vector<std::optional<MidoriIRFunctionId>> m_global_functions;
	std::vector<std::vector<bool>> m_self_captures;

public:
	explicit MidoriIRModuleFacts(const MidoriIRModule& module);

	// The function a global holds, when the top-level function defines it as
	// that function with nothing captured. A global is never set again.
	std::optional<MidoriIRFunctionId> GlobalFunction(MidoriIRGlobalSlot slot) const;

	// Whether every closure of `function` the module makes has this capture
	// bound to the closure itself, as a local function that names itself does.
	bool IsSelfCapture(MidoriIRFunctionId function, uint32_t capture) const;
};
