#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The verifier rules docs/midori-ir.md lists, numbered as there.
enum class MidoriIRRule : uint8_t
{
	Terminator = 1u,
	Dominance = 2u,
	SuccessorArguments = 3u,
	OperandTypes = 4u,
	GlobalSlot = 5u
};

struct MidoriIRViolation
{
	MidoriIRRule m_rule;
	std::string m_function;
	std::optional<MidoriIRBlockId> m_block;
	std::string m_message;

	MidoriIRViolation(MidoriIRRule rule, std::string function, std::optional<MidoriIRBlockId> block, std::string message);

	std::string ToString() const;
};

// A violation is a compiler bug, never a user error: lowering and every pass
// must leave the module valid.
class MidoriIRVerifier
{
private:
	const MidoriIRModule& m_module;

public:
	explicit MidoriIRVerifier(const MidoriIRModule& module);

	// Every violation found, in function and block order; empty when valid.
	std::vector<MidoriIRViolation> Verify() const;
};
