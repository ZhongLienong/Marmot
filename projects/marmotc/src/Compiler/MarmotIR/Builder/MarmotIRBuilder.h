#pragma once

#include "Compiler/MarmotIR/MarmotIR.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Appends to one function: creates its blocks and their parameters, and adds
// instructions to the block it is positioned at, stamped with the current
// source line. It checks nothing; the verifier does.
class MarmotIRBuilder
{
private:
	MarmotIRFunction& m_function;
	MarmotIRBlockId m_block;
	int m_line;

public:
	// Creates the entry block when the function has none, and positions there.
	explicit MarmotIRBuilder(MarmotIRFunction& function);

	MarmotIRBlockId CreateBlock();
	MarmotIRValueId AddParameter(MarmotIRBlockId block, std::shared_ptr<MarmotType> type, std::string name = {});

	MarmotIRBuilder& PositionAt(MarmotIRBlockId block);
	MarmotIRBuilder& AtLine(int line);
	MarmotIRBlockId CurrentBlock() const;

	MarmotIRValueId Emit(MarmotIROp op, std::shared_ptr<MarmotType> type, std::vector<MarmotIRValueId> operands = {}, MarmotIRImmediate immediate = {}, std::string name = {});
	// An op with a fixed signature, whose result type MarmotIROps.def gives.
	MarmotIRValueId Unary(MarmotIROp op, MarmotIRValueId operand, std::string name = {});
	MarmotIRValueId Binary(MarmotIROp op, MarmotIRValueId left, MarmotIRValueId right, std::string name = {});

	MarmotIRValueId ConstInt(int64_t value, std::string name = {});
	MarmotIRValueId ConstFloat(double value, std::string name = {});
	MarmotIRValueId ConstBool(bool value, std::string name = {});
	MarmotIRValueId ConstText(std::string value, std::string name = {});
	MarmotIRValueId ConstUnit(std::string name = {});

	void Jump(MarmotIRBlockId target, std::vector<MarmotIRValueId> arguments = {});
	void Branch(MarmotIRValueId condition, MarmotIRSuccessor if_true, MarmotIRSuccessor if_false);
	void Return(MarmotIRValueId value);
	// A direct call names a MarmotIRFunctionId, a call of a global a
	// MarmotIRGlobalSlot; with neither, the first operand is the closure called.
	void TailCall(MarmotIRImmediate callee, std::vector<MarmotIRValueId> operands);
	// Follows a call that returns Never.
	void Unreachable();

private:
	MarmotIRValueId NewValue(std::shared_ptr<MarmotType> type, std::string name);
	void Terminate(MarmotIROp op, std::vector<MarmotIRValueId> operands, MarmotIRImmediate immediate, std::vector<MarmotIRSuccessor> successors);
};
