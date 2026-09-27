#include "MidoriIRBuilder.h"

#include <utility>

MidoriIRBuilder::MidoriIRBuilder(MidoriIRFunction& function)
	: m_function(function),
	m_block(MidoriIRFunction::s_entry_block),
	m_line(0)
{
	if (m_function.m_blocks.empty())
	{
		m_function.m_blocks.emplace_back();
	}
}

MidoriIRBlockId MidoriIRBuilder::CreateBlock()
{
	m_function.m_blocks.emplace_back();
	return MidoriIRBlockId{ static_cast<uint32_t>(m_function.m_blocks.size() - 1u) };
}

MidoriIRValueId MidoriIRBuilder::AddParameter(MidoriIRBlockId block, std::shared_ptr<MidoriType> type, std::string name)
{
	const MidoriIRValueId parameter = NewValue(std::move(type), std::move(name));
	m_function.Block(block).m_parameters.push_back(parameter);
	return parameter;
}

MidoriIRBuilder& MidoriIRBuilder::PositionAt(MidoriIRBlockId block)
{
	m_block = block;
	return *this;
}

MidoriIRBuilder& MidoriIRBuilder::AtLine(int line)
{
	m_line = line;
	return *this;
}

MidoriIRBlockId MidoriIRBuilder::CurrentBlock() const
{
	return m_block;
}

bool MidoriIRBuilder::IsTerminated() const
{
	const std::vector<MidoriIRInstruction>& instructions = m_function.Block(m_block).m_instructions;
	return !instructions.empty() && IsMidoriIRTerminator(instructions.back().m_op);
}

MidoriIRValueId MidoriIRBuilder::Emit(MidoriIROp op, std::shared_ptr<MidoriType> type, std::vector<MidoriIRValueId> operands, MidoriIRImmediate immediate, std::string name)
{
	const MidoriIRValueId result = NewValue(type, std::move(name));
	m_function.Block(m_block).m_instructions.emplace_back(op, result, std::move(type), std::move(operands), std::move(immediate), std::vector<MidoriIRSuccessor>{}, m_line);
	return result;
}

MidoriIRValueId MidoriIRBuilder::Unary(MidoriIROp op, MidoriIRValueId operand, std::string name)
{
	return Emit(op, MidoriIRScalarType(GetMidoriIROpInfo(op).m_result.value()), { operand }, {}, std::move(name));
}

MidoriIRValueId MidoriIRBuilder::Binary(MidoriIROp op, MidoriIRValueId left, MidoriIRValueId right, std::string name)
{
	return Emit(op, MidoriIRScalarType(GetMidoriIROpInfo(op).m_result.value()), { left, right }, {}, std::move(name));
}

MidoriIRValueId MidoriIRBuilder::ConstInt(int64_t value, std::string name)
{
	return Emit(MidoriIROp::Const, MidoriIRScalarType(MidoriIRScalar::Int), {}, value, std::move(name));
}

MidoriIRValueId MidoriIRBuilder::ConstFloat(double value, std::string name)
{
	return Emit(MidoriIROp::Const, MidoriIRScalarType(MidoriIRScalar::Float), {}, value, std::move(name));
}

MidoriIRValueId MidoriIRBuilder::ConstBool(bool value, std::string name)
{
	return Emit(MidoriIROp::Const, MidoriIRScalarType(MidoriIRScalar::Bool), {}, value, std::move(name));
}

MidoriIRValueId MidoriIRBuilder::ConstText(std::string value, std::string name)
{
	return Emit(MidoriIROp::Const, MidoriIRScalarType(MidoriIRScalar::Text), {}, std::move(value), std::move(name));
}

MidoriIRValueId MidoriIRBuilder::ConstUnit(std::string name)
{
	return Emit(MidoriIROp::Const, MidoriType::MakeLiteralType<MidoriType::UnitType>(), {}, {}, std::move(name));
}

void MidoriIRBuilder::Jump(MidoriIRBlockId target, std::vector<MidoriIRValueId> arguments)
{
	Terminate(MidoriIROp::Jump, {}, {}, { MidoriIRSuccessor(target, std::move(arguments)) });
}

void MidoriIRBuilder::Branch(MidoriIRValueId condition, MidoriIRSuccessor if_true, MidoriIRSuccessor if_false)
{
	Terminate(MidoriIROp::Branch, { condition }, {}, { std::move(if_true), std::move(if_false) });
}

void MidoriIRBuilder::Return(MidoriIRValueId value)
{
	Terminate(MidoriIROp::Return, { value }, {}, {});
}

void MidoriIRBuilder::TailCall(MidoriIRImmediate callee, std::vector<MidoriIRValueId> operands)
{
	Terminate(MidoriIROp::TailCall, std::move(operands), std::move(callee), {});
}

void MidoriIRBuilder::Unreachable()
{
	Terminate(MidoriIROp::Unreachable, {}, {}, {});
}

MidoriIRValueId MidoriIRBuilder::NewValue(std::shared_ptr<MidoriType> type, std::string name)
{
	m_function.m_values.emplace_back(std::move(type), std::move(name));
	return MidoriIRValueId{ static_cast<uint32_t>(m_function.m_values.size() - 1u) };
}

void MidoriIRBuilder::Terminate(MidoriIROp op, std::vector<MidoriIRValueId> operands, MidoriIRImmediate immediate, std::vector<MidoriIRSuccessor> successors)
{
	m_function.Block(m_block).m_instructions.emplace_back(op, std::nullopt, MidoriType::MakeLiteralType<MidoriType::NeverType>(), std::move(operands), std::move(immediate), std::move(successors), m_line);
}
