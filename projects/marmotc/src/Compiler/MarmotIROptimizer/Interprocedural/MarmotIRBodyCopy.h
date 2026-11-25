#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"
#include "Support/Optional.h"

#include <cstdint>

// Where a function's blocks landed in the function they were copied into: at
// `m_first`, one after another in their old order.
struct MarmotIRCopiedBody
{
	uint32_t m_first;
	uint32_t m_count;

	MarmotIRBlockId Entry() const;
};

namespace MarmotIRBodyCopy
{
	// Copies every block of `source` into `target` at block `at`, moving the
	// blocks from there on up, with a fresh value for each of its values. The
	// entry's copy keeps parameters, which a jump into it passes. With a
	// line, every copied instruction takes it, as code copied from another
	// module's source must: its own lines are of another file.
	MarmotIRCopiedBody Copy(const MarmotIRFunction& source, MarmotIRFunction& target, uint32_t at, MarmotCompiler::Optional<int> line);
}
