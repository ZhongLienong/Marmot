#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The blocks of one function as a graph: its edges, the blocks the entry
// reaches in reverse postorder, and their dominators. A pass that changes the
// blocks builds it again.
class MidoriIRControlFlow
{
private:
	std::vector<std::vector<uint32_t>> m_successors;
	std::vector<std::vector<uint32_t>> m_predecessors;
	std::vector<uint32_t> m_reverse_postorder;
	std::vector<uint32_t> m_order;
	std::vector<std::optional<uint32_t>> m_immediate_dominators;

public:
	explicit MidoriIRControlFlow(const MidoriIRFunction& function);

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
struct MidoriIRSite
{
	uint32_t m_block;
	uint32_t m_position;
};

// An edge into a block: the terminator of `m_block` and which of its
// successors it is.
struct MidoriIREdge
{
	uint32_t m_block;
	size_t m_successor;
};

namespace MidoriIRAnalysis
{
	template<typename Transform>
	bool TransformFunctions(MidoriIRModule& module, const Transform& transform)
	{
		bool changed = false;
		for (MidoriIRFunction& function : module.m_functions)
		{
			changed = transform(function) || changed;
		}
		return changed;
	}

	std::vector<uint32_t> CountUses(const MidoriIRFunction& function);

	// Each block's incoming edges.
	std::vector<std::vector<MidoriIREdge>> IncomingEdges(const MidoriIRFunction& function);
	MidoriIRSuccessor& SuccessorOf(MidoriIRFunction& function, const MidoriIREdge& edge);

	// Where each value an instruction defines is defined; nothing for a block
	// parameter or a value no instruction defines.
	std::vector<std::optional<MidoriIRSite>> DefinitionSites(const MidoriIRFunction& function);

	// The instruction that defines a value, if one does.
	const MidoriIRInstruction* Definition(const MidoriIRFunction& function, const std::vector<std::optional<MidoriIRSite>>& sites, MidoriIRValueId value);

	// Replaces each use of a value with its replacement, following chains, in
	// every block, reachable or not.
	void ReplaceUses(MidoriIRFunction& function, const std::vector<std::optional<MidoriIRValueId>>& replacements);

	MidoriIRValueId AddValue(MidoriIRFunction& function, std::shared_ptr<MidoriType> type, std::string name = {});
	// An empty block at `index`, moving that block and every later one up by
	// one. The backend lays blocks out in order, so where a block goes is
	// where its code goes.
	MidoriIRBlockId InsertBlock(MidoriIRFunction& function, uint32_t index);

	// Drops the blocks the entry does not reach and numbers the rest in their
	// old order, the entry staying bb0.
	void RemoveUnreachableBlocks(MidoriIRFunction& function);

	MidoriIRInstruction Const(MidoriIRValueId result, std::shared_ptr<MidoriType> type, MidoriIRImmediate value, int line);
	MidoriIRInstruction Jump(MidoriIRSuccessor target, int line);
	MidoriIRInstruction Unreachable(int line);

	// The type a constant of this immediate has, which the verifier checks a
	// Const's type against: a newtype's constant is its representation's.
	std::shared_ptr<MidoriType> ConstantType(const MidoriIRImmediate& value, const std::shared_ptr<MidoriType>& fallback);

	// Floats compare by their bits: 0.0 and -0.0 are two constants, and a
	// NaN is itself.
	bool SameImmediate(const MidoriIRImmediate& left, const MidoriIRImmediate& right);

	// The number of instructions a function runs besides constants, which the
	// backend pushes where they are used.
	size_t Size(const MidoriIRFunction& function);

	// Whether an instruction may be dropped once nothing uses its value.
	bool IsRemovable(const MidoriIRFunction& function, const std::vector<std::optional<MidoriIRSite>>& sites, const MidoriIRInstruction& instruction);

	// Records proofs that an instruction cannot fault, so other passes can
	// move it as well as drop it.
	bool RefineEffects(MidoriIRFunction& function);

	// Whether a runtime error can never be raised in a function's own frame,
	// nor in a frame it makes: it neither fails, nor writes, nor calls
	// except in tail position, where the frame it would be seen in is gone
	// anyway. Copying such a body into its caller changes no stack trace.
	bool IsFrameTransparent(const MidoriIRFunction& function);

	// A global is defined once, by $main$, before anything can read it, and
	// never set again: reading one gives the same value wherever it is read.
	bool IsStableRead(const MidoriIRInstruction& instruction);
}
