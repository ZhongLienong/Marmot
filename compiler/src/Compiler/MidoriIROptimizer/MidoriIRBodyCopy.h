#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <cstdint>
#include <optional>

// Where a function's blocks landed in the function they were copied into: at
// `m_first`, one after another in their old order.
struct MidoriIRCopiedBody
{
	uint32_t m_first;
	uint32_t m_count;

	MidoriIRBlockId Entry() const;
};

namespace MidoriIRBodyCopy
{
	// Copies every block of `source` into `target` at block `at`, moving the
	// blocks from there on up, with a fresh value for each of its values. The
	// entry's copy keeps parameters, which a jump into it passes. With a
	// line, every copied instruction takes it, as code copied from another
	// module's source must: its own lines are of another file.
	MidoriIRCopiedBody Copy(const MidoriIRFunction& source, MidoriIRFunction& target, uint32_t at, std::optional<int> line);

	// The return type of what a call or tail call calls.
	std::shared_ptr<MidoriType> CalleeReturnType(const MidoriIRModule& module, const MidoriIRFunction& caller, const MidoriIRInstruction& call);

	// Turns a tail call into the call it makes, defining `result`: a
	// TailCall's callee is a function, a global or its first operand.
	MidoriIRInstruction AsCall(const MidoriIRInstruction& tail_call, MidoriIRValueId result, std::shared_ptr<MidoriType> type);
}
