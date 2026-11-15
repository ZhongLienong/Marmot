#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The verifier rules docs/marmot-ir.md lists, numbered as there.
enum class MarmotIRRule : uint8_t
{
	Terminator = 1u,
	Dominance = 2u,
	SuccessorArguments = 3u,
	OperandTypes = 4u,
	GlobalSlot = 5u
};

struct MarmotIRViolation
{
	MarmotIRRule m_rule;
	std::string m_function;
	std::optional<MarmotIRBlockId> m_block;
	std::string m_message;

	MarmotIRViolation(MarmotIRRule rule, std::string function, std::optional<MarmotIRBlockId> block, std::string message);

	std::string ToString() const;
};

// A violation is a compiler bug, never a user error: lowering and every pass
// must leave the module valid.
class MarmotIRVerifier
{
private:
	const MarmotIRModule& m_module;

public:
	explicit MarmotIRVerifier(const MarmotIRModule& module);

	// Every violation found, in function and block order; empty when valid.
	std::vector<MarmotIRViolation> Verify() const;
};
