#include "MarmotIR.h"
#include "Compiler/Lowering/GenericTypes.h"
#include "Support/Optional.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace
{
	constexpr size_t s_op_count = 0u
#define MARMOT_IR_UNARY(name, operand, result, effect, fault) + 1u
#define MARMOT_IR_BINARY(name, operand, result, effect, fault) + 1u
#define MARMOT_IR_SPECIAL(name, effect, fault) + 1u
#define MARMOT_IR_TERMINATOR(name) + 1u
#include "Compiler/MarmotIR/MarmotIROps.def"
		;

	const std::array<MarmotIROpInfo, s_op_count> s_op_infos =
	{
#define MARMOT_IR_UNARY(name, operand, result, effect, fault) MarmotIROpInfo{ #name, MarmotIRArity::Unary, MarmotIRScalar::operand, MarmotIRScalar::result, MarmotIREffect(MarmotIREffectKind::effect, MarmotIRFault::fault) },
#define MARMOT_IR_BINARY(name, operand, result, effect, fault) MarmotIROpInfo{ #name, MarmotIRArity::Binary, MarmotIRScalar::operand, MarmotIRScalar::result, MarmotIREffect(MarmotIREffectKind::effect, MarmotIRFault::fault) },
#define MARMOT_IR_SPECIAL(name, effect, fault) MarmotIROpInfo{ #name, MarmotIRArity::Special, MarmotCompiler::None, MarmotCompiler::None, MarmotIREffect(MarmotIREffectKind::effect, MarmotIRFault::fault) },
#define MARMOT_IR_TERMINATOR(name) MarmotIROpInfo{ #name, MarmotIRArity::Terminator, MarmotCompiler::None, MarmotCompiler::None, MarmotIREffect(MarmotIREffectKind::Io) },
#include "Compiler/MarmotIR/MarmotIROps.def"
	};
}

MarmotIREffect::MarmotIREffect(MarmotIREffectKind kind, MarmotIRFault fault)
	: m_kind(kind),
	m_fault(fault)
{
}

const MarmotIROpInfo& GetMarmotIROpInfo(MarmotIROp op)
{
	return s_op_infos[static_cast<size_t>(op)];
}

bool IsMarmotIRTerminator(MarmotIROp op)
{
	return GetMarmotIROpInfo(op).m_arity == MarmotIRArity::Terminator;
}

std::string_view MarmotIREffectName(MarmotIREffectKind kind)
{
	switch (kind)
	{
	case MarmotIREffectKind::Pure:
		return "pure";
	case MarmotIREffectKind::Fault:
		return "fault";
	case MarmotIREffectKind::Alloc:
		return "alloc";
	case MarmotIREffectKind::Read:
		return "read";
	case MarmotIREffectKind::Io:
		return "io";
	case MarmotIREffectKind::Call:
		return "call";
	}
	std::unreachable();
}

std::string_view MarmotIRFaultName(MarmotIRFault fault)
{
	switch (fault)
	{
	case MarmotIRFault::None:
		return "None";
	case MarmotIRFault::Index:
		return "Index";
	case MarmotIRFault::DivisionByZero:
		return "DivisionByZero";
	case MarmotIRFault::Conversion:
		return "Conversion";
	}
	std::unreachable();
}

const std::shared_ptr<MarmotType>& MarmotIRScalarType(MarmotIRScalar scalar)
{
	switch (scalar)
	{
	case MarmotIRScalar::Int:
		return MarmotType::MakeLiteralType<MarmotType::IntegerType>();
	case MarmotIRScalar::Float:
		return MarmotType::MakeLiteralType<MarmotType::FloatType>();
	case MarmotIRScalar::Byte:
		return MarmotType::MakeLiteralType<MarmotType::ByteType>();
	case MarmotIRScalar::Word:
		return MarmotType::MakeLiteralType<MarmotType::WordType>();
	case MarmotIRScalar::Bool:
		return MarmotType::MakeLiteralType<MarmotType::BoolType>();
	case MarmotIRScalar::Text:
		return MarmotType::MakeLiteralType<MarmotType::TextType>();
	}
	std::unreachable();
}

bool MarmotIRSameType(const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right)
{
	const std::shared_ptr<MarmotType> left_type = GenericTypes::RepresentationOf(left);
	const std::shared_ptr<MarmotType> right_type = GenericTypes::RepresentationOf(right);
	const auto same_all = [](const std::vector<std::shared_ptr<MarmotType>>& lefts, const std::vector<std::shared_ptr<MarmotType>>& rights)
	{
		return std::ranges::equal(lefts, rights, MarmotIRSameType);
	};

	if (left_type->IsType<MarmotType::ArrayType>() && right_type->IsType<MarmotType::ArrayType>())
	{
		return MarmotIRSameType(left_type->GetType<MarmotType::ArrayType>().m_element_type, right_type->GetType<MarmotType::ArrayType>().m_element_type);
	}
	if (left_type->IsType<MarmotType::TupleType>() && right_type->IsType<MarmotType::TupleType>())
	{
		return same_all(left_type->GetType<MarmotType::TupleType>().m_element_types, right_type->GetType<MarmotType::TupleType>().m_element_types);
	}
	if (left_type->IsType<MarmotType::FunctionType>() && right_type->IsType<MarmotType::FunctionType>())
	{
		const MarmotType::FunctionType& left_function = left_type->GetType<MarmotType::FunctionType>();
		const MarmotType::FunctionType& right_function = right_type->GetType<MarmotType::FunctionType>();
		return same_all(left_function.m_param_types, right_function.m_param_types) && MarmotIRSameType(left_function.m_return_type, right_function.m_return_type);
	}
	if (left_type->IsType<MarmotType::CellType>() && right_type->IsType<MarmotType::CellType>())
	{
		return MarmotIRSameType(left_type->GetType<MarmotType::CellType>().m_element_type, right_type->GetType<MarmotType::CellType>().m_element_type);
	}
	if (left_type->IsType<MarmotType::RangeType>() && right_type->IsType<MarmotType::RangeType>())
	{
		return MarmotIRSameType(left_type->GetType<MarmotType::RangeType>().m_element_type, right_type->GetType<MarmotType::RangeType>().m_element_type);
	}
	if (left_type->IsType<MarmotType::WorkerType>() && right_type->IsType<MarmotType::WorkerType>())
	{
		return MarmotIRSameType(left_type->GetType<MarmotType::WorkerType>().m_result_type, right_type->GetType<MarmotType::WorkerType>().m_result_type);
	}
	if (left_type->IsType<MarmotType::ChannelType>() && right_type->IsType<MarmotType::ChannelType>())
	{
		return MarmotIRSameType(left_type->GetType<MarmotType::ChannelType>().m_element_type, right_type->GetType<MarmotType::ChannelType>().m_element_type);
	}
	return *left_type == *right_type;
}

MarmotIRSuccessor::MarmotIRSuccessor(MarmotIRBlockId block, std::vector<MarmotIRValueId> arguments)
	: m_block(block),
	m_arguments(std::move(arguments))
{
}

MarmotIRInstruction::MarmotIRInstruction(MarmotIROp op, MarmotCompiler::Optional<MarmotIRValueId> result, std::shared_ptr<MarmotType> type, std::vector<MarmotIRValueId> operands, MarmotIRImmediate immediate, std::vector<MarmotIRSuccessor> successors, int line)
	: m_op(op),
	m_result(result),
	m_type(std::move(type)),
	m_operands(std::move(operands)),
	m_immediate(std::move(immediate)),
	m_successors(std::move(successors)),
	m_effect(GetMarmotIROpInfo(op).m_effect),
	m_line(line)
{
}

MarmotIRValue::MarmotIRValue(std::shared_ptr<MarmotType> type, std::string name)
	: m_type(std::move(type)),
	m_name(std::move(name))
{
}

MarmotIRFunction::MarmotIRFunction(std::string name, std::shared_ptr<MarmotType> return_type, std::vector<std::shared_ptr<MarmotType>> capture_types)
	: m_name(std::move(name)),
	m_capture_types(std::move(capture_types)),
	m_return_type(std::move(return_type))
{
}

const MarmotIRBlock& MarmotIRFunction::Block(MarmotIRBlockId block) const
{
	return m_blocks[block.m_index];
}

MarmotIRBlock& MarmotIRFunction::Block(MarmotIRBlockId block)
{
	return m_blocks[block.m_index];
}

const MarmotIRValue& MarmotIRFunction::Value(MarmotIRValueId value) const
{
	return m_values[value.m_index];
}

const std::shared_ptr<MarmotType>& MarmotIRFunction::TypeOf(MarmotIRValueId value) const
{
	return Value(value).m_type;
}

std::vector<std::shared_ptr<MarmotType>> MarmotIRFunction::ParameterTypes() const
{
	return Block(s_entry_block).m_parameters
		| std::views::transform([this](MarmotIRValueId parameter) { return TypeOf(parameter); })
		| std::ranges::to<std::vector>();
}

MarmotIRGlobal::MarmotIRGlobal(std::string name, std::shared_ptr<MarmotType> type, std::string module)
	: m_name(std::move(name)),
	m_type(std::move(type)),
	m_module(std::move(module))
{
}

bool MarmotIRGlobal::IsImported() const
{
	return !m_module.empty();
}

MarmotIRModule::MarmotIRModule(std::string name)
	: m_name(std::move(name))
{
}

MarmotIRGlobalSlot MarmotIRModule::ReserveGlobal(std::string name, std::shared_ptr<MarmotType> type)
{
	m_globals.emplace_back(std::move(name), std::move(type));
	return MarmotIRGlobalSlot{ static_cast<uint32_t>(m_globals.size() - 1u) };
}

MarmotIRGlobalSlot MarmotIRModule::ReserveImport(std::string module, std::string name, std::shared_ptr<MarmotType> type)
{
	m_globals.emplace_back(std::move(name), std::move(type), std::move(module));
	return MarmotIRGlobalSlot{ static_cast<uint32_t>(m_globals.size() - 1u) };
}

MarmotIRFunctionId MarmotIRModule::AddFunction(MarmotIRFunction function)
{
	m_functions.emplace_back(std::move(function));
	return MarmotIRFunctionId{ static_cast<uint32_t>(m_functions.size() - 1u) };
}

const MarmotIRFunction& MarmotIRModule::Function(MarmotIRFunctionId function) const
{
	return m_functions[function.m_index];
}

MarmotIRFunction& MarmotIRModule::Function(MarmotIRFunctionId function)
{
	return m_functions[function.m_index];
}
