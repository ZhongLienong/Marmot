#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The blocks of one function as a graph: its edges, the blocks the entry
// reaches in reverse postorder, and their dominators. A pass that changes the
// blocks builds it again.
class MarmotIRControlFlow
{
private:
	std::vector<std::vector<uint32_t>> m_successors;
	std::vector<std::vector<uint32_t>> m_predecessors;
	std::vector<uint32_t> m_reverse_postorder;
	std::vector<uint32_t> m_order;
	std::vector<std::optional<uint32_t>> m_immediate_dominators;

public:
	explicit MarmotIRControlFlow(const MarmotIRFunction& function);

	// Each successor once, in the terminator's order.
	const std::vector<uint32_t>& Successors(uint32_t block) const;
	// The reachable blocks that go to this one, each once.
	const std::vector<uint32_t>& Predecessors(uint32_t block) const;
	const std::vector<uint32_t>& ReversePostorder() const;
	bool IsReachable(uint32_t block) const;
	bool Dominates(uint32_t dominator, uint32_t block) const;
	// Each reachable block's children in the dominator tree.
	std::vector<std::vector<uint32_t>> DominatorTree() const;
};

// Where an instruction is: its block and its position there.
struct MarmotIRSite
{
	uint32_t m_block;
	uint32_t m_position;
};

// An edge into a block: the terminator of `m_block` and which of its
// successors it is.
struct MarmotIREdge
{
	uint32_t m_block;
	size_t m_successor;
};

namespace MarmotIRAnalysis
{
	template<typename Transform>
	bool TransformFunctions(MarmotIRModule& module, const Transform& transform)
	{
		bool changed = false;
		for (MarmotIRFunction& function : module.m_functions)
		{
			changed = transform(function) || changed;
		}
		return changed;
	}

	std::vector<uint32_t> CountUses(const MarmotIRFunction& function);

	// Each block's incoming edges.
	std::vector<std::vector<MarmotIREdge>> IncomingEdges(const MarmotIRFunction& function);
	MarmotIRSuccessor& SuccessorOf(MarmotIRFunction& function, const MarmotIREdge& edge);

	// Where each value an instruction defines is defined; nothing for a block
	// parameter or a value no instruction defines.
	std::vector<std::optional<MarmotIRSite>> DefinitionSites(const MarmotIRFunction& function);

	// The instruction that defines a value, if one does.
	const MarmotIRInstruction* Definition(const MarmotIRFunction& function, const std::vector<std::optional<MarmotIRSite>>& sites, MarmotIRValueId value);

	// Replaces each use of a value with its replacement, following chains, in
	// every block, reachable or not.
	void ReplaceUses(MarmotIRFunction& function, const std::vector<std::optional<MarmotIRValueId>>& replacements);

	MarmotIRValueId AddValue(MarmotIRFunction& function, std::shared_ptr<MarmotType> type, std::string name = {});
	// An empty block at `index`, moving that block and every later one up by
	// one. The backend lays blocks out in order, so where a block goes is
	// where its code goes.
	MarmotIRBlockId InsertBlock(MarmotIRFunction& function, uint32_t index);

	// Drops the blocks the entry does not reach and numbers the rest in their
	// old order, the entry staying bb0.
	void RemoveUnreachableBlocks(MarmotIRFunction& function);

	MarmotIRInstruction Const(MarmotIRValueId result, std::shared_ptr<MarmotType> type, MarmotIRImmediate value, int line);
	MarmotIRInstruction Jump(MarmotIRSuccessor target, int line);
	MarmotIRInstruction Unreachable(int line);

	// The type a constant of this immediate has, which the verifier checks a
	// Const's type against: a newtype's constant is its representation's.
	std::shared_ptr<MarmotType> ConstantType(const MarmotIRImmediate& value, const std::shared_ptr<MarmotType>& fallback);

	// Floats compare by their bits: 0.0 and -0.0 are two constants, and a
	// NaN is itself.
	bool SameImmediate(const MarmotIRImmediate& left, const MarmotIRImmediate& right);

	// The number of instructions a function runs besides constants, which the
	// backend pushes where they are used.
	size_t Size(const MarmotIRFunction& function);

	// Whether an instruction may be dropped once nothing uses its value.
	bool IsRemovable(const MarmotIRFunction& function, const std::vector<std::optional<MarmotIRSite>>& sites, const MarmotIRInstruction& instruction);

	// Records proofs that an instruction cannot fault, so other passes can
	// move it as well as drop it.
	bool RefineEffects(MarmotIRFunction& function);

	// Whether a runtime error can never be raised in a function's own frame,
	// nor in a frame it makes: it neither fails, nor writes, nor calls
	// except in tail position, where the frame it would be seen in is gone
	// anyway. Copying such a body into its caller changes no stack trace.
	bool IsFrameTransparent(const MarmotIRFunction& function);

	// A global is defined once, by $main$, before anything can read it, and
	// never set again: reading one gives the same value wherever it is read.
	bool IsStableRead(const MarmotIRInstruction& instruction);
}
