#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Support/Optional.h"

#include <algorithm>
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// MarmotIR: typed SSA, one graph per function, built after monomorphization.
// Blocks take parameters instead of phi nodes. docs/marmot-ir.md is the
// reference; change it with the IR.

struct MarmotIRValueId
{
	uint32_t m_index;

	auto operator<=>(const MarmotIRValueId&) const = default;
};

struct MarmotIRBlockId
{
	uint32_t m_index;

	auto operator<=>(const MarmotIRBlockId&) const = default;
};

struct MarmotIRFunctionId
{
	uint32_t m_index;

	auto operator<=>(const MarmotIRFunctionId&) const = default;
};

enum class MarmotIROp : uint8_t
{
#define MARMOT_IR_UNARY(name, operand, result, effect, fault) name,
#define MARMOT_IR_BINARY(name, operand, result, effect, fault) name,
#define MARMOT_IR_SPECIAL(name, effect, fault) name,
#define MARMOT_IR_TERMINATOR(name) name,
#include "Compiler/MarmotIR/MarmotIROps.def"
};

// DCE may drop Pure, Alloc and Read. Read may not move across an Io or Call,
// because what it reads may be written there.
enum class MarmotIREffectKind : uint8_t
{
	Pure,
	Fault,
	Alloc,
	Read,
	Io,
	Call
};

enum class MarmotIRFault : uint8_t
{
	None,
	Index,
	DivisionByZero,
	Conversion
};

struct MarmotIREffect
{
	MarmotIREffectKind m_kind;
	MarmotIRFault m_fault;

	MarmotIREffect(MarmotIREffectKind kind = MarmotIREffectKind::Pure, MarmotIRFault fault = MarmotIRFault::None);

	bool operator==(const MarmotIREffect&) const = default;
};

// The scalar types a fixed signature in MarmotIROps.def names.
enum class MarmotIRScalar : uint8_t
{
	Int,
	Float,
	Byte,
	Word,
	Bool,
	Text
};

enum class MarmotIRArity : uint8_t
{
	Unary,
	Binary,
	Special,
	Terminator
};

struct MarmotIROpInfo
{
	std::string_view m_name;
	MarmotIRArity m_arity;
	MarmotCompiler::Optional<MarmotIRScalar> m_operand;
	MarmotCompiler::Optional<MarmotIRScalar> m_result;
	MarmotIREffect m_effect;
};

const MarmotIROpInfo& GetMarmotIROpInfo(MarmotIROp op);

bool IsMarmotIRTerminator(MarmotIROp op);

std::string_view MarmotIREffectName(MarmotIREffectKind kind);

std::string_view MarmotIRFaultName(MarmotIRFault fault);

const std::shared_ptr<MarmotType>& MarmotIRScalarType(MarmotIRScalar scalar);

// Whether two types are one at run time. A newtype is its representation, so
// a conversion between them is no instruction at all.
bool MarmotIRSameType(const std::shared_ptr<MarmotType>& left, const std::shared_ptr<MarmotType>& right);

struct MarmotIRByte
{
	uint8_t m_value;

	bool operator==(const MarmotIRByte&) const = default;
};

struct MarmotIRWord
{
	uint64_t m_value;

	bool operator==(const MarmotIRWord&) const = default;
};

// A tuple element, a struct member, a union field or a capture, by position.
struct MarmotIRIndex
{
	uint32_t m_value;

	bool operator==(const MarmotIRIndex&) const = default;
};

struct MarmotIRTag
{
	int m_value;

	bool operator==(const MarmotIRTag&) const = default;
};

// A field of one union member: which member, and which of its fields.
struct MarmotIRUnionField
{
	int m_tag;
	uint32_t m_index;

	bool operator==(const MarmotIRUnionField&) const = default;
};

struct MarmotIRGlobalSlot
{
	uint32_t m_value;

	bool operator==(const MarmotIRGlobalSlot&) const = default;
};

struct MarmotIRForeign
{
	std::string m_name;

	bool operator==(const MarmotIRForeign&) const = default;
};

// Int, Float, Bool and Text constants are held as themselves; the rest are
// wrapped so that no two meanings share an alternative.
using MarmotIRImmediate = std::variant
<
	std::monostate,
	int64_t,
	double,
	bool,
	std::string,
	MarmotIRByte,
	MarmotIRWord,
	MarmotIRIndex,
	MarmotIRTag,
	MarmotIRUnionField,
	MarmotIRGlobalSlot,
	MarmotIRFunctionId,
	MarmotIRForeign
>;

// Where a terminator goes, with the arguments it passes to that block's
// parameters.
struct MarmotIRSuccessor
{
	MarmotIRBlockId m_block;
	std::vector<MarmotIRValueId> m_arguments;

	MarmotIRSuccessor(MarmotIRBlockId block, std::vector<MarmotIRValueId> arguments = {});

	bool operator==(const MarmotIRSuccessor&) const = default;
};

// Every instruction but a terminator defines exactly one value, of type
// m_type; an instruction run only for its effect defines a Unit.
struct MarmotIRInstruction
{
	MarmotIROp m_op;
	MarmotCompiler::Optional<MarmotIRValueId> m_result;
	std::shared_ptr<MarmotType> m_type;
	std::vector<MarmotIRValueId> m_operands;
	MarmotIRImmediate m_immediate;
	std::vector<MarmotIRSuccessor> m_successors;
	MarmotIREffect m_effect;
	int m_line;

	MarmotIRInstruction(MarmotIROp op, MarmotCompiler::Optional<MarmotIRValueId> result, std::shared_ptr<MarmotType> type, std::vector<MarmotIRValueId> operands, MarmotIRImmediate immediate, std::vector<MarmotIRSuccessor> successors, int line);
};

// Every value an instruction reads: its operands, then its successors'
// arguments. The instruction may be const or not.
template<typename Instruction, typename Visit>
void ForEachMarmotIRUse(Instruction& instruction, const Visit& visit)
{
	std::ranges::for_each(instruction.m_operands, visit);
	std::ranges::for_each(instruction.m_successors, [&visit](auto& successor) { std::ranges::for_each(successor.m_arguments, visit); });
}

struct MarmotIRBlock
{
	std::vector<MarmotIRValueId> m_parameters;
	std::vector<MarmotIRInstruction> m_instructions;
};

// What a value is. Where it is defined is found from the blocks, so a pass
// that moves an instruction has nothing else to update.
struct MarmotIRValue
{
	std::shared_ptr<MarmotType> m_type;
	std::string m_name;

	MarmotIRValue(std::shared_ptr<MarmotType> type, std::string name);
};

// The entry block's parameters are the function's parameters. Captures are
// read with GetCapture. A function whose body another module wrote, a
// specialization of that module's generic, names it in m_source_module, so a
// runtime error in it points at that module's file.
struct MarmotIRFunction
{
	std::string m_name;
	std::string m_source_module;
	std::vector<std::shared_ptr<MarmotType>> m_capture_types;
	std::shared_ptr<MarmotType> m_return_type;
	std::vector<MarmotIRBlock> m_blocks;
	std::vector<MarmotIRValue> m_values;

	MarmotIRFunction(std::string name, std::shared_ptr<MarmotType> return_type, std::vector<std::shared_ptr<MarmotType>> capture_types = {});

	static constexpr MarmotIRBlockId s_entry_block{ 0u };

	const MarmotIRBlock& Block(MarmotIRBlockId block) const;
	MarmotIRBlock& Block(MarmotIRBlockId block);
	const MarmotIRValue& Value(MarmotIRValueId value) const;
	const std::shared_ptr<MarmotType>& TypeOf(MarmotIRValueId value) const;
	std::vector<std::shared_ptr<MarmotType>> ParameterTypes() const;
};

// A global this module defines, or, when m_module names another module, one
// it imports from there, which it may read and call but not define or set.
struct MarmotIRGlobal
{
	std::string m_name;
	std::shared_ptr<MarmotType> m_type;
	std::string m_module;

	MarmotIRGlobal(std::string name, std::shared_ptr<MarmotType> type, std::string module = {});

	bool IsImported() const;
};

// One module: each specialization, instance method and lambda is its own
// function, and m_top_level runs the module's top-level statements. A global's
// slot is its index in m_globals.
struct MarmotIRModule
{
	std::string m_name;
	std::vector<MarmotIRGlobal> m_globals;
	std::vector<MarmotIRFunction> m_functions;
	MarmotCompiler::Optional<MarmotIRFunctionId> m_top_level;

	explicit MarmotIRModule(std::string name);

	MarmotIRGlobalSlot ReserveGlobal(std::string name, std::shared_ptr<MarmotType> type);
	MarmotIRGlobalSlot ReserveImport(std::string module, std::string name, std::shared_ptr<MarmotType> type);
	MarmotIRFunctionId AddFunction(MarmotIRFunction function);
	const MarmotIRFunction& Function(MarmotIRFunctionId function) const;
	MarmotIRFunction& Function(MarmotIRFunctionId function);
};
