#pragma once

#include "Compiler/AbstractSyntaxTree/Type.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// MidoriIR: typed SSA, one graph per function, built after monomorphization.
// Blocks take parameters instead of phi nodes. docs/midori-ir.md is the
// reference; change it with the IR.

struct MidoriIRValueId
{
	uint32_t m_index;

	auto operator<=>(const MidoriIRValueId&) const = default;
};

struct MidoriIRBlockId
{
	uint32_t m_index;

	auto operator<=>(const MidoriIRBlockId&) const = default;
};

struct MidoriIRFunctionId
{
	uint32_t m_index;

	auto operator<=>(const MidoriIRFunctionId&) const = default;
};

enum class MidoriIROp : uint8_t
{
#define MIDORI_IR_UNARY(name, operand, result, effect, fault) name,
#define MIDORI_IR_BINARY(name, operand, result, effect, fault) name,
#define MIDORI_IR_SPECIAL(name, effect, fault) name,
#define MIDORI_IR_TERMINATOR(name) name,
#include "Compiler/MidoriIR/MidoriIROps.def"
};

// DCE may drop Pure, Alloc and Read. Read may not move across an Io or Call,
// because what it reads may be written there.
enum class MidoriIREffectKind : uint8_t
{
	Pure,
	Fault,
	Alloc,
	Read,
	Io,
	Call
};

enum class MidoriIRFault : uint8_t
{
	None,
	Index,
	DivisionByZero,
	Conversion
};

struct MidoriIREffect
{
	MidoriIREffectKind m_kind;
	MidoriIRFault m_fault;

	MidoriIREffect(MidoriIREffectKind kind = MidoriIREffectKind::Pure, MidoriIRFault fault = MidoriIRFault::None);

	bool operator==(const MidoriIREffect&) const = default;
};

// The scalar types a fixed signature in MidoriIROps.def names.
enum class MidoriIRScalar : uint8_t
{
	Int,
	Float,
	Byte,
	Word,
	Bool,
	Text
};

enum class MidoriIRArity : uint8_t
{
	Unary,
	Binary,
	Special,
	Terminator
};

struct MidoriIROpInfo
{
	std::string_view m_name;
	MidoriIRArity m_arity;
	std::optional<MidoriIRScalar> m_operand;
	std::optional<MidoriIRScalar> m_result;
	MidoriIREffect m_effect;
};

const MidoriIROpInfo& GetMidoriIROpInfo(MidoriIROp op);

bool IsMidoriIRTerminator(MidoriIROp op);

std::string_view MidoriIREffectName(MidoriIREffectKind kind);

std::string_view MidoriIRFaultName(MidoriIRFault fault);

const std::shared_ptr<MidoriType>& MidoriIRScalarType(MidoriIRScalar scalar);

// Whether two types are one at run time. A newtype is its representation, so
// a conversion between them is no instruction at all.
bool MidoriIRSameType(const std::shared_ptr<MidoriType>& left, const std::shared_ptr<MidoriType>& right);

struct MidoriIRByte
{
	uint8_t m_value;

	bool operator==(const MidoriIRByte&) const = default;
};

struct MidoriIRWord
{
	uint64_t m_value;

	bool operator==(const MidoriIRWord&) const = default;
};

// A tuple element, a struct member, a union field or a capture, by position.
struct MidoriIRIndex
{
	uint32_t m_value;

	bool operator==(const MidoriIRIndex&) const = default;
};

struct MidoriIRTag
{
	int m_value;

	bool operator==(const MidoriIRTag&) const = default;
};

// A field of one union member: which member, and which of its fields.
struct MidoriIRUnionField
{
	int m_tag;
	uint32_t m_index;

	bool operator==(const MidoriIRUnionField&) const = default;
};

struct MidoriIRGlobalSlot
{
	uint32_t m_value;

	bool operator==(const MidoriIRGlobalSlot&) const = default;
};

// The union tags `join` builds its Result<T, WorkerError> from.
struct MidoriIRJoinTags
{
	int m_ok;
	int m_err;
	int m_cancelled;
	int m_failed;

	bool operator==(const MidoriIRJoinTags&) const = default;
};

struct MidoriIRForeign
{
	std::string m_name;

	bool operator==(const MidoriIRForeign&) const = default;
};

// Int, Float, Bool and Text constants are held as themselves; the rest are
// wrapped so that no two meanings share an alternative.
using MidoriIRImmediate = std::variant
<
	std::monostate,
	int64_t,
	double,
	bool,
	std::string,
	MidoriIRByte,
	MidoriIRWord,
	MidoriIRIndex,
	MidoriIRTag,
	MidoriIRUnionField,
	MidoriIRGlobalSlot,
	MidoriIRFunctionId,
	MidoriIRForeign,
	MidoriIRJoinTags
>;

// Where a terminator goes, with the arguments it passes to that block's
// parameters.
struct MidoriIRSuccessor
{
	MidoriIRBlockId m_block;
	std::vector<MidoriIRValueId> m_arguments;

	MidoriIRSuccessor(MidoriIRBlockId block, std::vector<MidoriIRValueId> arguments = {});
};

// Every instruction but a terminator defines exactly one value, of type
// m_type; an instruction run only for its effect defines a Unit.
struct MidoriIRInstruction
{
	MidoriIROp m_op;
	std::optional<MidoriIRValueId> m_result;
	std::shared_ptr<MidoriType> m_type;
	std::vector<MidoriIRValueId> m_operands;
	MidoriIRImmediate m_immediate;
	std::vector<MidoriIRSuccessor> m_successors;
	MidoriIREffect m_effect;
	int m_line;

	MidoriIRInstruction(MidoriIROp op, std::optional<MidoriIRValueId> result, std::shared_ptr<MidoriType> type, std::vector<MidoriIRValueId> operands, MidoriIRImmediate immediate, std::vector<MidoriIRSuccessor> successors, int line);
};

struct MidoriIRBlock
{
	std::vector<MidoriIRValueId> m_parameters;
	std::vector<MidoriIRInstruction> m_instructions;
};

// What a value is. Where it is defined is found from the blocks, so a pass
// that moves an instruction has nothing else to update.
struct MidoriIRValue
{
	std::shared_ptr<MidoriType> m_type;
	std::string m_name;

	MidoriIRValue(std::shared_ptr<MidoriType> type, std::string name);
};

// The entry block's parameters are the function's parameters. Captures are
// read with GetCapture. A function whose body another module wrote, a
// specialization of that module's generic, names it in m_source_module, so a
// runtime error in it points at that module's file.
struct MidoriIRFunction
{
	std::string m_name;
	std::string m_source_module;
	std::vector<std::shared_ptr<MidoriType>> m_capture_types;
	std::shared_ptr<MidoriType> m_return_type;
	std::vector<MidoriIRBlock> m_blocks;
	std::vector<MidoriIRValue> m_values;

	MidoriIRFunction(std::string name, std::shared_ptr<MidoriType> return_type, std::vector<std::shared_ptr<MidoriType>> capture_types = {});

	static constexpr MidoriIRBlockId s_entry_block{ 0u };

	const MidoriIRBlock& Block(MidoriIRBlockId block) const;
	MidoriIRBlock& Block(MidoriIRBlockId block);
	const MidoriIRValue& Value(MidoriIRValueId value) const;
	const std::shared_ptr<MidoriType>& TypeOf(MidoriIRValueId value) const;
	std::vector<std::shared_ptr<MidoriType>> ParameterTypes() const;
};

// A global this module defines, or, when m_module names another module, one
// it imports from there, which it may read and call but not define or set.
struct MidoriIRGlobal
{
	std::string m_name;
	std::shared_ptr<MidoriType> m_type;
	std::string m_module;

	MidoriIRGlobal(std::string name, std::shared_ptr<MidoriType> type, std::string module = {});

	bool IsImported() const;
};

// One module: each specialization, instance method and lambda is its own
// function, and m_top_level runs the module's top-level statements. A global's
// slot is its index in m_globals.
struct MidoriIRModule
{
	std::string m_name;
	std::vector<MidoriIRGlobal> m_globals;
	std::vector<MidoriIRFunction> m_functions;
	std::optional<MidoriIRFunctionId> m_top_level;

	explicit MidoriIRModule(std::string name);

	MidoriIRGlobalSlot ReserveGlobal(std::string name, std::shared_ptr<MidoriType> type);
	MidoriIRGlobalSlot ReserveImport(std::string module, std::string name, std::shared_ptr<MidoriType> type);
	MidoriIRFunctionId AddFunction(MidoriIRFunction function);
	const MidoriIRFunction& Function(MidoriIRFunctionId function) const;
	MidoriIRFunction& Function(MidoriIRFunctionId function);
};
