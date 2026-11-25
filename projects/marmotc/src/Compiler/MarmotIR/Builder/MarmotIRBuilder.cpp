#include "MarmotIRBuilder.h"
#include "Support/Optional.h"

#include <utility>

MarmotIRBuilder::MarmotIRBuilder(MarmotIRFunction& function)
	: m_function(function),
	m_block(MarmotIRFunction::s_entry_block),
	m_line(0)
{
	if (m_function.m_blocks.empty())
	{
		m_function.m_blocks.emplace_back();
	}
}

MarmotIRBlockId MarmotIRBuilder::CreateBlock()
{
	m_function.m_blocks.emplace_back();
	return MarmotIRBlockId{ static_cast<uint32_t>(m_function.m_blocks.size() - 1u) };
}

MarmotIRValueId MarmotIRBuilder::AddParameter(MarmotIRBlockId block, std::shared_ptr<MarmotType> type, std::string name)
{
	const MarmotIRValueId parameter = NewValue(std::move(type), std::move(name));
	m_function.Block(block).m_parameters.push_back(parameter);
	return parameter;
}

MarmotIRBuilder& MarmotIRBuilder::PositionAt(MarmotIRBlockId block)
{
	m_block = block;
	return *this;
}

MarmotIRBuilder& MarmotIRBuilder::AtLine(int line)
{
	m_line = line;
	return *this;
}

MarmotIRBlockId MarmotIRBuilder::CurrentBlock() const
{
	return m_block;
}

MarmotIRValueId MarmotIRBuilder::Emit(MarmotIROp op, std::shared_ptr<MarmotType> type, std::vector<MarmotIRValueId> operands, MarmotIRImmediate immediate, std::string name)
{
	const MarmotIRValueId result = NewValue(type, std::move(name));
	m_function.Block(m_block).m_instructions.emplace_back(op, result, std::move(type), std::move(operands), std::move(immediate), std::vector<MarmotIRSuccessor>{}, m_line);
	return result;
}

MarmotIRValueId MarmotIRBuilder::Unary(MarmotIROp op, MarmotIRValueId operand, std::string name)
{
	return Emit(op, MarmotIRScalarType(GetMarmotIROpInfo(op).m_result.Value()), { operand }, {}, std::move(name));
}

MarmotIRValueId MarmotIRBuilder::Binary(MarmotIROp op, MarmotIRValueId left, MarmotIRValueId right, std::string name)
{
	return Emit(op, MarmotIRScalarType(GetMarmotIROpInfo(op).m_result.Value()), { left, right }, {}, std::move(name));
}

MarmotIRValueId MarmotIRBuilder::ConstInt(int64_t value, std::string name)
{
	return Emit(MarmotIROp::Const, MarmotIRScalarType(MarmotIRScalar::Int), {}, value, std::move(name));
}

MarmotIRValueId MarmotIRBuilder::ConstFloat(double value, std::string name)
{
	return Emit(MarmotIROp::Const, MarmotIRScalarType(MarmotIRScalar::Float), {}, value, std::move(name));
}

MarmotIRValueId MarmotIRBuilder::ConstBool(bool value, std::string name)
{
	return Emit(MarmotIROp::Const, MarmotIRScalarType(MarmotIRScalar::Bool), {}, value, std::move(name));
}

MarmotIRValueId MarmotIRBuilder::ConstText(std::string value, std::string name)
{
	return Emit(MarmotIROp::Const, MarmotIRScalarType(MarmotIRScalar::Text), {}, std::move(value), std::move(name));
}

MarmotIRValueId MarmotIRBuilder::ConstUnit(std::string name)
{
	return Emit(MarmotIROp::Const, MarmotType::MakeLiteralType<MarmotType::UnitType>(), {}, {}, std::move(name));
}

void MarmotIRBuilder::Jump(MarmotIRBlockId target, std::vector<MarmotIRValueId> arguments)
{
	Terminate(MarmotIROp::Jump, {}, {}, { MarmotIRSuccessor(target, std::move(arguments)) });
}

void MarmotIRBuilder::Branch(MarmotIRValueId condition, MarmotIRSuccessor if_true, MarmotIRSuccessor if_false)
{
	Terminate(MarmotIROp::Branch, { condition }, {}, { std::move(if_true), std::move(if_false) });
}

void MarmotIRBuilder::Return(MarmotIRValueId value)
{
	Terminate(MarmotIROp::Return, { value }, {}, {});
}

void MarmotIRBuilder::TailCall(MarmotIRImmediate callee, std::vector<MarmotIRValueId> operands)
{
	Terminate(MarmotIROp::TailCall, std::move(operands), std::move(callee), {});
}

void MarmotIRBuilder::Unreachable()
{
	Terminate(MarmotIROp::Unreachable, {}, {}, {});
}

MarmotIRValueId MarmotIRBuilder::NewValue(std::shared_ptr<MarmotType> type, std::string name)
{
	m_function.m_values.emplace_back(std::move(type), std::move(name));
	return MarmotIRValueId{ static_cast<uint32_t>(m_function.m_values.size() - 1u) };
}

void MarmotIRBuilder::Terminate(MarmotIROp op, std::vector<MarmotIRValueId> operands, MarmotIRImmediate immediate, std::vector<MarmotIRSuccessor> successors)
{
	m_function.Block(m_block).m_instructions.emplace_back(op, MarmotCompiler::None, MarmotType::MakeLiteralType<MarmotType::NeverType>(), std::move(operands), std::move(immediate), std::move(successors), m_line);
}
