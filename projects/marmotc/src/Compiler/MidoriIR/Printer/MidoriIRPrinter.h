#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <string>
#include <vector>

// The textual form docs/midori-ir.md describes, which --emit-ir prints.
class MidoriIRPrinter
{
private:
	const MidoriIRModule& m_module;

public:
	explicit MidoriIRPrinter(const MidoriIRModule& module);

	std::string Print() const;
	std::string PrintFunction(const MidoriIRFunction& function) const;

private:
	// A value prints as its name when no other value in the function shares
	// it, as name.N when one does, and as %N when it has none.
	static std::vector<std::string> ValueNames(const MidoriIRFunction& function);
	std::string PrintImmediate(const MidoriIRImmediate& immediate) const;
	std::string PrintInstruction(const MidoriIRInstruction& instruction, const std::vector<std::string>& names) const;
	static std::string PrintSuccessor(const MidoriIRSuccessor& successor, const std::vector<std::string>& names);
};
