#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"

#include <string>
#include <vector>

// The textual form docs/marmot-ir.md describes, which --emit-ir prints.
class MarmotIRPrinter
{
private:
	const MarmotIRModule& m_module;

public:
	explicit MarmotIRPrinter(const MarmotIRModule& module);

	std::string Print() const;
	std::string PrintFunction(const MarmotIRFunction& function) const;

private:
	// A value prints as its name when no other value in the function shares
	// it, as name.N when one does, and as %N when it has none.
	static std::vector<std::string> ValueNames(const MarmotIRFunction& function);
	std::string PrintImmediate(const MarmotIRImmediate& immediate) const;
	std::string PrintInstruction(const MarmotIRInstruction& instruction, const std::vector<std::string>& names) const;
	static std::string PrintSuccessor(const MarmotIRSuccessor& successor, const std::vector<std::string>& names);
};
