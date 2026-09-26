#include "MidoriIR.h"

#include <array>
#include <ranges>
#include <utility>

namespace
{
	constexpr size_t s_op_count = 0u
#define MIDORI_IR_UNARY(name, operand, result, effect, fault) + 1u
#define MIDORI_IR_BINARY(name, operand, result, effect, fault) + 1u
#define MIDORI_IR_SPECIAL(name, effect, fault) + 1u
#define MIDORI_IR_TERMINATOR(name) + 1u
#include "Compiler/MidoriIR/MidoriIROps.def"
		;

	const std::array<MidoriIROpInfo, s_op_count> s_op_infos =
	{
#define MIDORI_IR_UNARY(name, operand, result, effect, fault) MidoriIROpInfo{ #name, MidoriIRArity::Unary, MidoriIRScalar::operand, MidoriIRScalar::result, MidoriIREffect(MidoriIREffectKind::effect, MidoriIRFault::fault) },
#define MIDORI_IR_BINARY(name, operand, result, effect, fault) MidoriIROpInfo{ #name, MidoriIRArity::Binary, MidoriIRScalar::operand, MidoriIRScalar::result, MidoriIREffect(MidoriIREffectKind::effect, MidoriIRFault::fault) },
#define MIDORI_IR_SPECIAL(name, effect, fault) MidoriIROpInfo{ #name, MidoriIRArity::Special, std::nullopt, std::nullopt, MidoriIREffect(MidoriIREffectKind::effect, MidoriIRFault::fault) },
#define MIDORI_IR_TERMINATOR(name) MidoriIROpInfo{ #name, MidoriIRArity::Terminator, std::nullopt, std::nullopt, MidoriIREffect(MidoriIREffectKind::Io) },
#include "Compiler/MidoriIR/MidoriIROps.def"
	};
}

MidoriIREffect::MidoriIREffect(MidoriIREffectKind kind, MidoriIRFault fault)
	: m_kind(kind),
	m_fault(fault)
{
}

const MidoriIROpInfo& GetMidoriIROpInfo(MidoriIROp op)
{
	return s_op_infos[static_cast<size_t>(op)];
}

bool IsMidoriIRTerminator(MidoriIROp op)
{
	return GetMidoriIROpInfo(op).m_arity == MidoriIRArity::Terminator;
}

std::string_view MidoriIREffectName(MidoriIREffectKind kind)
{
	switch (kind)
	{
	case MidoriIREffectKind::Pure:
		return "pure";
	case MidoriIREffectKind::Fault:
		return "fault";
	case MidoriIREffectKind::Alloc:
		return "alloc";
	case MidoriIREffectKind::Read:
		return "read";
	case MidoriIREffectKind::Io:
		return "io";
	case MidoriIREffectKind::Call:
		return "call";
	}
	std::unreachable();
}

std::string_view MidoriIRFaultName(MidoriIRFault fault)
{
	switch (fault)
	{
	case MidoriIRFault::None:
		return "None";
	case MidoriIRFault::Index:
		return "Index";
	case MidoriIRFault::DivisionByZero:
		return "DivisionByZero";
	case MidoriIRFault::Conversion:
		return "Conversion";
	}
	std::unreachable();
}

const std::shared_ptr<MidoriType>& MidoriIRScalarType(MidoriIRScalar scalar)
{
	switch (scalar)
	{
	case MidoriIRScalar::Int:
		return MidoriType::MakeLiteralType<MidoriType::IntegerType>();
	case MidoriIRScalar::Float:
		return MidoriType::MakeLiteralType<MidoriType::FloatType>();
	case MidoriIRScalar::Byte:
		return MidoriType::MakeLiteralType<MidoriType::ByteType>();
	case MidoriIRScalar::Word:
		return MidoriType::MakeLiteralType<MidoriType::WordType>();
	case MidoriIRScalar::Bool:
		return MidoriType::MakeLiteralType<MidoriType::BoolType>();
	case MidoriIRScalar::Text:
		return MidoriType::MakeLiteralType<MidoriType::TextType>();
	}
	std::unreachable();
}

MidoriIRSuccessor::MidoriIRSuccessor(MidoriIRBlockId block, std::vector<MidoriIRValueId> arguments, std::optional<int> case_tag)
	: m_block(block),
	m_arguments(std::move(arguments)),
	m_case_tag(case_tag)
{
}

MidoriIRInstruction::MidoriIRInstruction(MidoriIROp op, std::optional<MidoriIRValueId> result, std::shared_ptr<MidoriType> type, std::vector<MidoriIRValueId> operands, MidoriIRImmediate immediate, std::vector<MidoriIRSuccessor> successors, int line)
	: m_op(op),
	m_result(result),
	m_type(std::move(type)),
	m_operands(std::move(operands)),
	m_immediate(std::move(immediate)),
	m_successors(std::move(successors)),
	m_effect(GetMidoriIROpInfo(op).m_effect),
	m_line(line)
{
}

MidoriIRValue::MidoriIRValue(std::shared_ptr<MidoriType> type, std::string name)
	: m_type(std::move(type)),
	m_name(std::move(name))
{
}

MidoriIRFunction::MidoriIRFunction(std::string name, std::shared_ptr<MidoriType> return_type, std::vector<std::shared_ptr<MidoriType>> capture_types)
	: m_name(std::move(name)),
	m_capture_types(std::move(capture_types)),
	m_return_type(std::move(return_type))
{
}

const MidoriIRBlock& MidoriIRFunction::Block(MidoriIRBlockId block) const
{
	return m_blocks[block.m_index];
}

MidoriIRBlock& MidoriIRFunction::Block(MidoriIRBlockId block)
{
	return m_blocks[block.m_index];
}

const MidoriIRValue& MidoriIRFunction::Value(MidoriIRValueId value) const
{
	return m_values[value.m_index];
}

const std::shared_ptr<MidoriType>& MidoriIRFunction::TypeOf(MidoriIRValueId value) const
{
	return Value(value).m_type;
}

std::vector<std::shared_ptr<MidoriType>> MidoriIRFunction::ParameterTypes() const
{
	return Block(s_entry_block).m_parameters
		| std::views::transform([this](MidoriIRValueId parameter) { return TypeOf(parameter); })
		| std::ranges::to<std::vector>();
}

MidoriIRGlobal::MidoriIRGlobal(std::string name, std::shared_ptr<MidoriType> type, std::string module)
	: m_name(std::move(name)),
	m_type(std::move(type)),
	m_module(std::move(module))
{
}

bool MidoriIRGlobal::IsImported() const
{
	return !m_module.empty();
}

MidoriIRModule::MidoriIRModule(std::string name)
	: m_name(std::move(name))
{
}

MidoriIRGlobalSlot MidoriIRModule::ReserveGlobal(std::string name, std::shared_ptr<MidoriType> type)
{
	m_globals.emplace_back(std::move(name), std::move(type));
	return MidoriIRGlobalSlot{ static_cast<uint32_t>(m_globals.size() - 1u) };
}

MidoriIRGlobalSlot MidoriIRModule::ReserveImport(std::string module, std::string name, std::shared_ptr<MidoriType> type)
{
	m_globals.emplace_back(std::move(name), std::move(type), std::move(module));
	return MidoriIRGlobalSlot{ static_cast<uint32_t>(m_globals.size() - 1u) };
}

MidoriIRFunctionId MidoriIRModule::AddFunction(MidoriIRFunction function)
{
	m_functions.emplace_back(std::move(function));
	return MidoriIRFunctionId{ static_cast<uint32_t>(m_functions.size() - 1u) };
}

const MidoriIRFunction& MidoriIRModule::Function(MidoriIRFunctionId function) const
{
	return m_functions[function.m_index];
}

MidoriIRFunction& MidoriIRModule::Function(MidoriIRFunctionId function)
{
	return m_functions[function.m_index];
}
