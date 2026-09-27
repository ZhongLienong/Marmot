#pragma once

#include "Compiler/MidoriIR/MidoriIR.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Appends to one function: creates its blocks and their parameters, and adds
// instructions to the block it is positioned at, stamped with the current
// source line. It checks nothing; the verifier does.
class MidoriIRBuilder
{
private:
	MidoriIRFunction& m_function;
	MidoriIRBlockId m_block;
	int m_line;

public:
	// Creates the entry block when the function has none, and positions there.
	explicit MidoriIRBuilder(MidoriIRFunction& function);

	MidoriIRBlockId CreateBlock();
	MidoriIRValueId AddParameter(MidoriIRBlockId block, std::shared_ptr<MidoriType> type, std::string name = {});

	MidoriIRBuilder& PositionAt(MidoriIRBlockId block);
	MidoriIRBuilder& AtLine(int line);
	MidoriIRBlockId CurrentBlock() const;
	bool IsTerminated() const;

	MidoriIRValueId Emit(MidoriIROp op, std::shared_ptr<MidoriType> type, std::vector<MidoriIRValueId> operands = {}, MidoriIRImmediate immediate = {}, std::string name = {});
	// An op with a fixed signature, whose result type MidoriIROps.def gives.
	MidoriIRValueId Unary(MidoriIROp op, MidoriIRValueId operand, std::string name = {});
	MidoriIRValueId Binary(MidoriIROp op, MidoriIRValueId left, MidoriIRValueId right, std::string name = {});

	MidoriIRValueId ConstInt(int64_t value, std::string name = {});
	MidoriIRValueId ConstFloat(double value, std::string name = {});
	MidoriIRValueId ConstBool(bool value, std::string name = {});
	MidoriIRValueId ConstText(std::string value, std::string name = {});
	MidoriIRValueId ConstUnit(std::string name = {});

	void Jump(MidoriIRBlockId target, std::vector<MidoriIRValueId> arguments = {});
	void Branch(MidoriIRValueId condition, MidoriIRSuccessor if_true, MidoriIRSuccessor if_false);
	// Each case carries its tag; without a default the cases must cover every member.
	void Switch(MidoriIRValueId tag, std::vector<MidoriIRSuccessor> cases, std::optional<MidoriIRSuccessor> otherwise = std::nullopt);
	void Return(MidoriIRValueId value);
	// A direct call names a MidoriIRFunctionId, a call of a global a
	// MidoriIRGlobalSlot; with neither, the first operand is the closure called.
	void TailCall(MidoriIRImmediate callee, std::vector<MidoriIRValueId> operands);
	// Follows a call that returns Never.
	void Unreachable();

private:
	MidoriIRValueId NewValue(std::shared_ptr<MidoriType> type, std::string name);
	void Terminate(MidoriIROp op, std::vector<MidoriIRValueId> operands, MidoriIRImmediate immediate, std::vector<MidoriIRSuccessor> successors);
};
