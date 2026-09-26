#include "BytecodeBackend.h"
#include "Common/Builtins/BuiltinTable.h"
#include "Common/Constant/Constant.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <format>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	// Where a value lives between its definition and its uses.
	enum class Storage : uint8_t
	{
		// A frame slot, stored once and read at each use.
		Slot,
		// The operand stack, where its definition left it for its only use.
		Stack,
		// Nowhere: a constant, pushed again at each use.
		Rematerialized,
		// Nowhere: nothing uses it.
		Discarded
	};

	std::optional<OpCode> ScalarOpCode(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case AddInt: return OpCode::ADD_INTEGER;
		case SubInt: return OpCode::SUBTRACT_INTEGER;
		case MulInt: return OpCode::MULTIPLY_INTEGER;
		case DivInt: return OpCode::DIVIDE_INTEGER;
		case ModInt: return OpCode::MODULO_INTEGER;
		case NegInt: return OpCode::NEGATE_INTEGER;
		case AddFloat: return OpCode::ADD_FLOAT;
		case SubFloat: return OpCode::SUBTRACT_FLOAT;
		case MulFloat: return OpCode::MULTIPLY_FLOAT;
		case DivFloat: return OpCode::DIVIDE_FLOAT;
		case ModFloat: return OpCode::MODULO_FLOAT;
		case NegFloat: return OpCode::NEGATE_FLOAT;
		case AddByte: return OpCode::ADD_BYTE;
		case SubByte: return OpCode::SUBTRACT_BYTE;
		case MulByte: return OpCode::MULTIPLY_BYTE;
		case DivByte: return OpCode::DIVIDE_BYTE;
		case ModByte: return OpCode::MODULO_BYTE;
		case AddWord: return OpCode::ADD_WORD;
		case SubWord: return OpCode::SUBTRACT_WORD;
		case MulWord: return OpCode::MULTIPLY_WORD;
		case DivWord: return OpCode::DIVIDE_WORD;
		case ModWord: return OpCode::MODULO_WORD;
		case BitAndInt:
		case BitAndByte:
		case BitAndWord:
			return OpCode::BITWISE_AND;
		case BitOrInt:
		case BitOrByte:
		case BitOrWord:
			return OpCode::BITWISE_OR;
		case BitXorInt:
		case BitXorByte:
		case BitXorWord:
			return OpCode::BITWISE_XOR;
		case BitNotInt:
		case BitNotWord:
			return OpCode::BITWISE_NOT;
		case ShlInt: return OpCode::LEFT_SHIFT;
		case ShrInt: return OpCode::RIGHT_SHIFT;
		case ShlByte: return OpCode::LEFT_SHIFT_BYTE;
		case ShrByte: return OpCode::RIGHT_SHIFT_BYTE;
		case ShlWord: return OpCode::LEFT_SHIFT_WORD;
		case ShrWord: return OpCode::RIGHT_SHIFT_WORD;
		case EqInt:
		case EqBool:
			return OpCode::EQUAL_INTEGER;
		case NeInt:
		case NeBool:
			return OpCode::NOT_EQUAL_INTEGER;
		case LtInt: return OpCode::LESS_INTEGER;
		case LeInt: return OpCode::LESS_EQUAL_INTEGER;
		case GtInt: return OpCode::GREATER_INTEGER;
		case GeInt: return OpCode::GREATER_EQUAL_INTEGER;
		case EqFloat: return OpCode::EQUAL_FLOAT;
		case NeFloat: return OpCode::NOT_EQUAL_FLOAT;
		case LtFloat: return OpCode::LESS_FLOAT;
		case LeFloat: return OpCode::LESS_EQUAL_FLOAT;
		case GtFloat: return OpCode::GREATER_FLOAT;
		case GeFloat: return OpCode::GREATER_EQUAL_FLOAT;
		case EqByte: return OpCode::EQUAL_BYTE;
		case NeByte: return OpCode::NOT_EQUAL_BYTE;
		case LtByte: return OpCode::LESS_BYTE;
		case LeByte: return OpCode::LESS_EQUAL_BYTE;
		case GtByte: return OpCode::GREATER_BYTE;
		case GeByte: return OpCode::GREATER_EQUAL_BYTE;
		case EqWord: return OpCode::EQUAL_WORD;
		case NeWord: return OpCode::NOT_EQUAL_WORD;
		case LtWord: return OpCode::LESS_WORD;
		case LeWord: return OpCode::LESS_EQUAL_WORD;
		case GtWord: return OpCode::GREATER_WORD;
		case GeWord: return OpCode::GREATER_EQUAL_WORD;
		case NotBool: return OpCode::NOT;
		case EqText: return OpCode::EQUAL_TEXT;
		case IntToFloat: return OpCode::INT_TO_FLOAT;
		case FloatToInt: return OpCode::FLOAT_TO_INT;
		case IntToText: return OpCode::INT_TO_TEXT;
		case FloatToText: return OpCode::FLOAT_TO_TEXT;
		case WordToText: return OpCode::WORD_TO_TEXT;
		case TextToInt: return OpCode::TEXT_TO_INT;
		case TextToFloat: return OpCode::TEXT_TO_FLOAT;
		case ByteToInt: return OpCode::BYTE_TO_INT;
		case IntToByte: return OpCode::INT_TO_BYTE;
		case ByteToWord: return OpCode::BYTE_TO_WORD;
		case WordToByte: return OpCode::WORD_TO_BYTE;
		case WordToInt: return OpCode::WORD_TO_INT;
		case IntToWord: return OpCode::INT_TO_WORD;
		case ByteToFloat: return OpCode::BYTE_TO_FLOAT;
		case FloatToByte: return OpCode::FLOAT_TO_BYTE;
		case WordToFloat: return OpCode::WORD_TO_FLOAT;
		case FloatToWord: return OpCode::FLOAT_TO_WORD;
		default:
			return std::nullopt;
		}
	}

	std::optional<OpCode> SmallIntOpCode(int64_t value)
	{
		switch (value)
		{
		case -1: return OpCode::INT_MINUS_1;
		case 0: return OpCode::INT_0;
		case 1: return OpCode::INT_1;
		case 2: return OpCode::INT_2;
		case 3: return OpCode::INT_3;
		case 4: return OpCode::INT_4;
		case 5: return OpCode::INT_5;
		case 10: return OpCode::INT_10;
		default: return std::nullopt;
		}
	}

	// The wide form of a variable operation, for an index past one byte.
	OpCode WideOpCode(OpCode op)
	{
		switch (op)
		{
		case OpCode::DEFINE_GLOBAL: return OpCode::DEFINE_GLOBAL_WIDE;
		case OpCode::GET_GLOBAL: return OpCode::GET_GLOBAL_WIDE;
		case OpCode::SET_GLOBAL: return OpCode::SET_GLOBAL_WIDE;
		case OpCode::GET_LOCAL: return OpCode::GET_LOCAL_WIDE;
		case OpCode::SET_LOCAL: return OpCode::SET_LOCAL_WIDE;
		case OpCode::CALL_GLOBAL: return OpCode::CALL_GLOBAL_WIDE;
		default: throw std::logic_error(std::format("opcode {} has no wide form", static_cast<int>(op)));
		}
	}

	bool IsUnit(const TypeRef& type)
	{
		return type->IsType<MidoriType::UnitType>();
	}

	// What a foreign call's result is, so the VM knows how to take it back.
	uint8_t ForeignReturnTag(const TypeRef& type)
	{
		if (type->IsType<MidoriType::TextType>())
		{
			return 1u;
		}
		if (type->IsType<MidoriType::ArrayType>())
		{
			return 2u;
		}
		return 0u;
	}

	bool CallsClosureOperand(const MidoriIRInstruction& instruction)
	{
		return instruction.m_op == MidoriIROp::CallValue || (instruction.m_op == MidoriIROp::TailCall && std::holds_alternative<std::monostate>(instruction.m_immediate));
	}

	// Whether the instruction leaves its value on the stack.
	bool PushesResult(const MidoriIRInstruction& instruction)
	{
		return !IsMidoriIRTerminator(instruction.m_op) && instruction.m_op != MidoriIROp::GlobalDefine && instruction.m_op != MidoriIROp::GlobalSet;
	}

	class FunctionEmitter
	{
	private:
		struct Fixup
		{
			int m_operand;
			MidoriIRBlockId m_target;
			int m_line;
		};

		using Emitted = std::expected<void, CompilerError>;

		BytecodeBackend& m_backend;
		const MidoriIRFunction& m_function;
		BytecodeStream m_stream;
		std::vector<Storage> m_storage;
		std::vector<int> m_slots;
		std::vector<const MidoriIRInstruction*> m_definitions;
		std::vector<int> m_block_offsets;
		std::vector<Fixup> m_fixups;
		int m_slot_count = 0;

	public:
		FunctionEmitter(BytecodeBackend& backend, const MidoriIRFunction& function)
			: m_backend(backend),
			m_function(function),
			m_storage(function.m_values.size(), Storage::Slot),
			m_slots(function.m_values.size(), -1),
			m_definitions(function.m_values.size(), nullptr),
			m_block_offsets(function.m_blocks.size(), -1)
		{
		}

		std::expected<BytecodeStream, CompilerError> Emit() &&
		{
			AssignStorage();
			ScheduleStack();
			AssignSlots();
			if (m_slot_count - 1 > MAX_VARIABLES)
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many variables (max {})", MAX_VARIABLES), FirstLine()));
			}

			const int arity = static_cast<int>(m_function.Block(MidoriIRFunction::s_entry_block).m_parameters.size());
			for (int slot = arity; slot < m_slot_count; slot += 1)
			{
				EmitByte(OpCode::PUSH_PLACEHOLDER, FirstLine());
			}

			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				m_block_offsets[block] = m_stream.GetByteCodeSize();
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					const Emitted emitted = EmitInstruction(MidoriIRBlockId{ block }, instruction);
					if (!emitted.has_value())
					{
						return std::unexpected(emitted.error());
					}
				}
			}

			for (const Fixup& fixup : m_fixups)
			{
				const Emitted patched = PatchJump(fixup.m_operand, m_block_offsets[fixup.m_target.m_index], fixup.m_line);
				if (!patched.has_value())
				{
					return std::unexpected(patched.error());
				}
			}
			return std::move(m_stream);
		}

	private:
		int FirstLine() const
		{
			const std::vector<MidoriIRInstruction>& entry = m_function.Block(MidoriIRFunction::s_entry_block).m_instructions;
			return entry.empty() ? 0 : entry.front().m_line;
		}

		const MidoriIRBlock& TargetOf(const MidoriIRSuccessor& successor) const
		{
			return m_function.Block(successor.m_block);
		}

		// The values an instruction needs on the stack, bottom first. A call
		// of a closure takes it on top of its arguments; a jump takes the
		// arguments it stores into its target's parameters.
		std::vector<MidoriIRValueId> PushSequence(const MidoriIRInstruction& instruction) const
		{
			if (CallsClosureOperand(instruction))
			{
				std::vector<MidoriIRValueId> sequence(instruction.m_operands.begin() + 1, instruction.m_operands.end());
				sequence.push_back(instruction.m_operands.front());
				return sequence;
			}
			if (instruction.m_op == MidoriIROp::Jump)
			{
				return MoveArguments(instruction.m_successors.front());
			}
			return instruction.m_operands;
		}

		// A successor's arguments that its parameters keep; a Unit parameter
		// keeps nothing.
		std::vector<MidoriIRValueId> MoveArguments(const MidoriIRSuccessor& successor) const
		{
			const std::vector<MidoriIRValueId>& parameters = TargetOf(successor).m_parameters;
			std::vector<MidoriIRValueId> arguments;
			for (size_t index = 0u; index < parameters.size(); index += 1u)
			{
				if (m_storage[parameters[index].m_index] == Storage::Slot)
				{
					arguments.push_back(successor.m_arguments[index]);
				}
			}
			return arguments;
		}

		void AssignStorage()
		{
			const size_t count = m_function.m_values.size();
			std::vector<uint32_t> uses(count, 0u);
			std::vector<std::optional<uint32_t>> use_blocks(count);
			std::vector<std::optional<uint32_t>> definition_blocks(count);
			std::vector<bool> is_parameter(count, false);
			std::vector<bool> is_edge_argument(count, false);

			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				for (const MidoriIRValueId parameter : m_function.m_blocks[block].m_parameters)
				{
					is_parameter[parameter.m_index] = true;
				}
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					if (instruction.m_result.has_value())
					{
						definition_blocks[instruction.m_result->m_index] = block;
						m_definitions[instruction.m_result->m_index] = &instruction;
					}
					for (const MidoriIRValueId operand : instruction.m_operands)
					{
						uses[operand.m_index] += 1u;
						use_blocks[operand.m_index] = block;
					}
					for (const MidoriIRSuccessor& successor : instruction.m_successors)
					{
						for (const MidoriIRValueId argument : successor.m_arguments)
						{
							uses[argument.m_index] += 1u;
							use_blocks[argument.m_index] = block;
							is_edge_argument[argument.m_index] = is_edge_argument[argument.m_index] || instruction.m_op != MidoriIROp::Jump;
						}
					}
				}
			}

			for (size_t value = 0u; value < count; value += 1u)
			{
				const MidoriIRInstruction* definition = m_definitions[value];
				const bool is_constant = definition != nullptr && definition->m_op == MidoriIROp::Const && !definition->m_type->IsType<MidoriType::TextType>();
				if (IsUnit(m_function.m_values[value].m_type) || is_constant)
				{
					m_storage[value] = Storage::Rematerialized;
				}
				else if (is_parameter[value])
				{
					m_storage[value] = Storage::Slot;
				}
				else if (uses[value] == 0u)
				{
					m_storage[value] = Storage::Discarded;
				}
				else if (uses[value] == 1u && use_blocks[value] == definition_blocks[value] && !is_edge_argument[value])
				{
					m_storage[value] = Storage::Stack;
				}
			}
		}

		// A value can stay on the stack only if, when its use runs, it is where
		// that use takes it: the stack values an instruction takes must come
		// first among its operands, and be the top of the stack in order. One
		// that is not moves to a slot; it was stored when it was made, so it
		// leaves the stack it was on, and the use is checked again.
		void ScheduleStack()
		{
			for (const MidoriIRBlock& block : m_function.m_blocks)
			{
				std::vector<MidoriIRValueId> pending;
				for (const MidoriIRInstruction& instruction : block.m_instructions)
				{
					const std::vector<MidoriIRValueId> sequence = PushSequence(instruction);
					std::vector<MidoriIRValueId> taken = StackValues(sequence);
					while (!IsTakenFromTop(sequence, taken, pending))
					{
						for (const MidoriIRValueId value : taken)
						{
							m_storage[value.m_index] = Storage::Slot;
							std::erase(pending, value);
						}
						taken = StackValues(sequence);
					}

					pending.resize(pending.size() - taken.size());
					if (instruction.m_result.has_value() && m_storage[instruction.m_result->m_index] == Storage::Stack)
					{
						pending.push_back(instruction.m_result.value());
					}
				}
			}
		}

		std::vector<MidoriIRValueId> StackValues(const std::vector<MidoriIRValueId>& values) const
		{
			return values
				| std::views::filter([this](MidoriIRValueId value) { return m_storage[value.m_index] == Storage::Stack; })
				| std::ranges::to<std::vector>();
		}

		static bool IsTakenFromTop(const std::vector<MidoriIRValueId>& sequence, const std::vector<MidoriIRValueId>& taken, const std::vector<MidoriIRValueId>& pending)
		{
			const bool is_prefix = std::ranges::equal(std::views::take(sequence, taken.size()), taken);
			const bool is_on_top = pending.size() >= taken.size() && std::ranges::equal(std::views::drop(pending, pending.size() - taken.size()), taken);
			return is_prefix && is_on_top;
		}

		void AssignSlots()
		{
			const std::vector<MidoriIRValueId>& parameters = m_function.Block(MidoriIRFunction::s_entry_block).m_parameters;
			for (const MidoriIRValueId parameter : parameters)
			{
				m_slots[parameter.m_index] = m_slot_count++;
			}
			for (size_t value = 0u; value < m_slots.size(); value += 1u)
			{
				if (m_storage[value] == Storage::Slot && m_slots[value] < 0)
				{
					m_slots[value] = m_slot_count++;
				}
			}
		}

		void EmitByte(OpCode byte, int line)
		{
			m_stream.AddByteCode(byte, line);
		}

		void EmitOperand(int value, int line)
		{
			m_stream.AddByteCode(static_cast<OpCode>(value & BYTE_MASK), line);
		}

		// An index of a local or a global, in one byte or, wide, in two, high first.
		void EmitVariable(OpCode op, int index, int line)
		{
			if (index <= MAX_LOCAL_VARIABLES)
			{
				EmitByte(op, line);
				EmitOperand(index, line);
				return;
			}
			EmitByte(WideOpCode(op), line);
			EmitOperand(index >> SHIFT_8_BITS, line);
			EmitOperand(index, line);
		}

		void EmitEightBytes(uint64_t value, int line)
		{
			for (int shift = 0; shift < 64; shift += 8)
			{
				EmitOperand(static_cast<int>((value >> shift) & BYTE_MASK), line);
			}
		}

		Emitted EmitConstant(const MidoriIRInstruction& constant, int line)
		{
			return std::visit([this, line](const auto& value) -> Emitted
			{
				using Immediate = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<Immediate, int64_t>)
				{
					const std::optional<OpCode> small = SmallIntOpCode(value);
					if (small.has_value())
					{
						EmitByte(small.value(), line);
						return {};
					}
					EmitByte(OpCode::INTEGER_CONSTANT, line);
					EmitEightBytes(static_cast<uint64_t>(value), line);
					return {};
				}
				else if constexpr (std::is_same_v<Immediate, double>)
				{
					EmitByte(OpCode::FLOAT_CONSTANT, line);
					EmitEightBytes(std::bit_cast<uint64_t>(value), line);
					return {};
				}
				else if constexpr (std::is_same_v<Immediate, bool>)
				{
					EmitByte(value ? OpCode::OP_TRUE : OpCode::OP_FALSE, line);
					return {};
				}
				else if constexpr (std::is_same_v<Immediate, std::string>)
				{
					return m_backend.TextConstant(value, line)
						.transform([this, line](int index)
						{
							EmitByte(OpCode::LOAD_STRING_WIDE, line);
							EmitOperand(index, line);
							EmitOperand(index >> SHIFT_8_BITS, line);
						});
				}
				else if constexpr (std::is_same_v<Immediate, MidoriIRByte>)
				{
					EmitByte(OpCode::BYTE_CONSTANT, line);
					EmitOperand(value.m_value, line);
					return {};
				}
				else if constexpr (std::is_same_v<Immediate, MidoriIRWord>)
				{
					EmitByte(OpCode::WORD_CONSTANT, line);
					EmitEightBytes(value.m_value, line);
					return {};
				}
				else if constexpr (std::is_same_v<Immediate, std::monostate>)
				{
					EmitByte(OpCode::OP_UNIT, line);
					return {};
				}
				else
				{
					throw std::logic_error("a constant with a non-constant immediate");
				}
			}, constant.m_immediate);
		}

		Emitted Load(MidoriIRValueId value, int line)
		{
			switch (m_storage[value.m_index])
			{
			case Storage::Stack:
				return {};
			case Storage::Slot:
				EmitVariable(OpCode::GET_LOCAL, m_slots[value.m_index], line);
				return {};
			case Storage::Rematerialized:
				if (IsUnit(m_function.m_values[value.m_index].m_type))
				{
					EmitByte(OpCode::OP_UNIT, line);
					return {};
				}
				return EmitConstant(*m_definitions[value.m_index], line);
			case Storage::Discarded:
				break;
			}
			throw std::logic_error("a use of a value that has none");
		}

		Emitted LoadAll(const std::vector<MidoriIRValueId>& values, int line)
		{
			for (const MidoriIRValueId value : values)
			{
				const Emitted loaded = Load(value, line);
				if (!loaded.has_value())
				{
					return loaded;
				}
			}
			return {};
		}

		void Store(MidoriIRValueId value, int line)
		{
			switch (m_storage[value.m_index])
			{
			case Storage::Stack:
				return;
			case Storage::Slot:
				EmitVariable(OpCode::SET_LOCAL, m_slots[value.m_index], line);
				EmitByte(OpCode::POP, line);
				return;
			case Storage::Rematerialized:
			case Storage::Discarded:
				EmitByte(OpCode::POP, line);
				return;
			}
		}

		Emitted EmitInstruction(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const int line = instruction.m_line;
			if (instruction.m_op == MidoriIROp::Const)
			{
				const Storage storage = m_storage[instruction.m_result->m_index];
				if (storage == Storage::Rematerialized || storage == Storage::Discarded)
				{
					return {};
				}
				return EmitConstant(instruction, line)
					.transform([this, &instruction, line]() { Store(instruction.m_result.value(), line); });
			}

			if (IsMidoriIRTerminator(instruction.m_op))
			{
				return EmitTerminator(block, instruction);
			}

			return LoadAll(PushSequence(instruction), line)
				.and_then([this, &instruction, line]() { return EmitOperation(instruction, line); })
				.transform([this, &instruction, line]()
				{
					if (PushesResult(instruction))
					{
						Store(instruction.m_result.value(), line);
					}
				});
		}

		Emitted CheckArity(size_t arity, int line) const
		{
			if (arity > static_cast<size_t>(MAX_FUNCTION_ARITY))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many arguments (max {})", MAX_FUNCTION_ARITY + 1), line));
			}
			return {};
		}

		std::expected<int, CompilerError> Procedure(MidoriIRFunctionId function, int line) const
		{
			const size_t procedure = m_backend.ProcedureIndex(function);
			if (procedure > static_cast<size_t>(MAX_FUNCTION_COUNT))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many functions (max {})", MAX_FUNCTION_COUNT + 1), line));
			}
			return static_cast<int>(procedure);
		}

		Emitted EmitCallOf(OpCode op, OpCode first_fixed, size_t arity, int line)
		{
			if (arity <= 3u)
			{
				EmitByte(static_cast<OpCode>(static_cast<int>(first_fixed) + static_cast<int>(arity)), line);
				return {};
			}
			EmitByte(op, line);
			EmitOperand(static_cast<int>(arity), line);
			return {};
		}

		Emitted EmitOperation(const MidoriIRInstruction& instruction, int line)
		{
			const std::optional<OpCode> scalar = ScalarOpCode(instruction.m_op);
			if (scalar.has_value())
			{
				EmitByte(scalar.value(), line);
				return {};
			}

			const size_t operand_count = instruction.m_operands.size();
			switch (instruction.m_op)
			{
			case MidoriIROp::NeText:
				EmitByte(OpCode::EQUAL_TEXT, line);
				EmitByte(OpCode::NOT, line);
				return {};
			// BITWISE_NOT flips all 64 bits; a Byte keeps the low eight.
			case MidoriIROp::BitNotByte:
				EmitByte(OpCode::BITWISE_NOT, line);
				EmitByte(OpCode::INT_TO_BYTE, line);
				return {};
			case MidoriIROp::Concat:
				EmitByte(instruction.m_type->IsType<MidoriType::TextType>() ? OpCode::CONCAT_TEXT : OpCode::CONCAT_ARRAY, line);
				return {};
			case MidoriIROp::Call:
				return CheckArity(operand_count, line)
					.and_then([&]() { return Procedure(std::get<MidoriIRFunctionId>(instruction.m_immediate), line); })
					.transform([&](int procedure)
					{
						if (operand_count <= 3u)
						{
							EmitByte(static_cast<OpCode>(static_cast<int>(OpCode::CALL_PROC_0) + static_cast<int>(operand_count)), line);
							EmitOperand(procedure, line);
							return;
						}
						EmitByte(OpCode::CALL_PROC, line);
						EmitOperand(procedure, line);
						EmitOperand(static_cast<int>(operand_count), line);
					});
			case MidoriIROp::CallGlobal:
				return CheckArity(operand_count, line)
					.transform([&]()
					{
						EmitVariable(OpCode::CALL_GLOBAL, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
						EmitOperand(static_cast<int>(operand_count), line);
					});
			case MidoriIROp::CallValue:
				return CheckArity(operand_count - 1u, line)
					.and_then([&]() { return EmitCallOf(OpCode::CALL, OpCode::CALL_0, operand_count - 1u, line); });
			case MidoriIROp::CallForeign:
				return CheckArity(operand_count, line)
					.transform([&]()
					{
						EmitByte(OpCode::CALL_FOREIGN_INDEXED, line);
						EmitOperand(static_cast<int>(MarmotBuiltins::FindIndex(std::get<MidoriIRForeign>(instruction.m_immediate).m_name).value()), line);
						EmitOperand(static_cast<int>(operand_count), line);
						EmitOperand(ForeignReturnTag(instruction.m_type), line);
					});
			case MidoriIROp::MakeClosure:
				if (operand_count != 0u)
				{
					throw std::logic_error("the bytecode backend cannot emit a closure with captures yet");
				}
				return Procedure(std::get<MidoriIRFunctionId>(instruction.m_immediate), line)
					.transform([&](int procedure)
					{
						EmitByte(OpCode::MAKE_FUNCTION, line);
						EmitOperand(procedure, line);
					});
			case MidoriIROp::GlobalDefine:
				EmitVariable(OpCode::DEFINE_GLOBAL, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				return {};
			case MidoriIROp::GlobalGet:
				EmitVariable(OpCode::GET_GLOBAL, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				return {};
			case MidoriIROp::GlobalSet:
				EmitVariable(OpCode::SET_GLOBAL, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				EmitByte(OpCode::POP, line);
				return {};
			default:
				throw std::logic_error(std::format("the bytecode backend cannot emit {} yet", GetMidoriIROpInfo(instruction.m_op).m_name));
			}
		}

		// Every argument is pushed before any parameter is stored, so a jump
		// that passes one parameter's value to another reads it first.
		Emitted EmitMoves(const MidoriIRSuccessor& successor, int line)
		{
			const std::vector<MidoriIRValueId> parameters = TargetOf(successor).m_parameters
				| std::views::filter([this](MidoriIRValueId parameter) { return m_storage[parameter.m_index] == Storage::Slot; })
				| std::ranges::to<std::vector>();
			return LoadAll(MoveArguments(successor), line)
				.transform([this, &parameters, line]()
				{
					for (const MidoriIRValueId parameter : parameters | std::views::reverse)
					{
						Store(parameter, line);
					}
				});
		}

		// A jump to the block laid out next is left out unless code follows it.
		Emitted EmitJumpTo(MidoriIRBlockId from, MidoriIRBlockId target, bool may_fall_through, int line)
		{
			if (may_fall_through && target.m_index == from.m_index + 1u)
			{
				return {};
			}
			if (target.m_index > from.m_index)
			{
				EmitByte(OpCode::JUMP, line);
				m_fixups.push_back(Fixup{ m_stream.GetByteCodeSize(), target, line });
				EmitOperand(0, line);
				EmitOperand(0, line);
				return {};
			}

			EmitByte(OpCode::JUMP_BACK, line);
			const int offset = m_stream.GetByteCodeSize() + 2 - m_block_offsets[target.m_index];
			if (offset > MAX_JUMP_SIZE)
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Loop body too large (max {})", MAX_JUMP_SIZE + 1), line));
			}
			EmitOperand(offset, line);
			EmitOperand(offset >> SHIFT_8_BITS, line);
			return {};
		}

		// A forward jump's offset counts from the end of its operand.
		Emitted PatchJump(int operand, int target, int line)
		{
			const int offset = target - operand - 2;
			if (offset > MAX_JUMP_SIZE)
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too much code to jump over (max {})", MAX_JUMP_SIZE + 1), line));
			}
			m_stream.SetByteCode(operand, static_cast<OpCode>(offset & BYTE_MASK));
			m_stream.SetByteCode(operand + 1, static_cast<OpCode>((offset >> SHIFT_8_BITS) & BYTE_MASK));
			return {};
		}

		Emitted EmitTerminator(MidoriIRBlockId block, const MidoriIRInstruction& instruction)
		{
			const int line = instruction.m_line;
			switch (instruction.m_op)
			{
			case MidoriIROp::Jump:
			{
				const MidoriIRSuccessor& target = instruction.m_successors.front();
				return EmitMoves(target, line)
					.and_then([&]() { return EmitJumpTo(block, target.m_block, true, line); });
			}
			case MidoriIROp::Branch:
			{
				// JUMP_IF_FALSE leaves the condition, so each path pops it.
				const MidoriIRSuccessor& if_true = instruction.m_successors[0u];
				const MidoriIRSuccessor& if_false = instruction.m_successors[1u];
				return Load(instruction.m_operands.front(), line)
					.and_then([&]() -> Emitted
					{
						EmitByte(OpCode::JUMP_IF_FALSE, line);
						const int when_false = m_stream.GetByteCodeSize();
						EmitOperand(0, line);
						EmitOperand(0, line);
						EmitByte(OpCode::POP, line);
						return EmitMoves(if_true, line)
							.and_then([&]() { return EmitJumpTo(block, if_true.m_block, false, line); })
							.and_then([&]() { return PatchJump(when_false, m_stream.GetByteCodeSize(), line); })
							.and_then([&]() -> Emitted
							{
								EmitByte(OpCode::POP, line);
								return EmitMoves(if_false, line);
							})
							.and_then([&]() { return EmitJumpTo(block, if_false.m_block, true, line); });
					});
			}
			case MidoriIROp::Return:
				return Load(instruction.m_operands.front(), line)
					.transform([&]() { EmitByte(OpCode::RETURN, line); });
			case MidoriIROp::TailCall:
				return EmitTailCall(instruction, line);
			case MidoriIROp::Halt:
				EmitByte(OpCode::HALT, line);
				return {};
			default:
				throw std::logic_error(std::format("the bytecode backend cannot emit {} yet", GetMidoriIROpInfo(instruction.m_op).m_name));
			}
		}

		// TAIL_CALL takes the callee on top of its arguments.
		Emitted EmitTailCall(const MidoriIRInstruction& instruction, int line)
		{
			const bool is_closure = CallsClosureOperand(instruction);
			const size_t arity = instruction.m_operands.size() - (is_closure ? 1u : 0u);
			return CheckArity(arity, line)
				.and_then([&]() { return LoadAll(PushSequence(instruction), line); })
				.and_then([&]() -> Emitted
				{
					if (std::holds_alternative<MidoriIRFunctionId>(instruction.m_immediate))
					{
						return Procedure(std::get<MidoriIRFunctionId>(instruction.m_immediate), line)
							.transform([&](int procedure)
							{
								EmitByte(OpCode::MAKE_FUNCTION, line);
								EmitOperand(procedure, line);
							});
					}
					if (std::holds_alternative<MidoriIRGlobalSlot>(instruction.m_immediate))
					{
						EmitVariable(OpCode::GET_GLOBAL, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
					}
					return {};
				})
				.transform([&]()
				{
					EmitByte(OpCode::TAIL_CALL, line);
					EmitOperand(static_cast<int>(arity), line);
				});
		}
	};
}

BytecodeBackend::BytecodeBackend(const LoweredModule& lowered, std::string_view file_name, const std::vector<std::string>& source_lines)
	: m_lowered(lowered),
	m_file_name(file_name),
	m_source_lines(source_lines)
{
}

MidoriResult::CodeGeneratorResult BytecodeBackend::Emit() &&
{
	const MidoriIRModule& module = m_lowered.m_module;
	const MidoriIRFunctionId top_level = module.m_top_level.value();

	// The top-level function is procedure 0, which the linker runs to
	// initialise the module; the others follow in order.
	m_procedure_indices.resize(module.m_functions.size());
	size_t next_procedure = 1u;
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		m_procedure_indices[function] = function == top_level.m_index ? 0u : next_procedure++;
	}

	std::expected<std::vector<BytecodeModule::ImportedSymbol>, CompilerError> imports = AssignGlobals();
	if (!imports.has_value())
	{
		return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(imports.error())));
	}

	BytecodeModule bytecode(module.m_name, std::filesystem::path(m_file_name));
	bytecode.m_procedures.resize(module.m_functions.size());
	bytecode.m_procedure_names.resize(module.m_functions.size());
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		const MidoriIRFunction& ir_function = module.m_functions[function];
		std::expected<BytecodeStream, CompilerError> procedure = FunctionEmitter(*this, ir_function).Emit();
		if (!procedure.has_value())
		{
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(procedure.error())));
		}
		const size_t index = m_procedure_indices[function];
		bytecode.m_procedures[index] = std::move(procedure).value();
		bytecode.m_procedure_names[index] = std::format("{}@{}", ir_function.m_name, module.m_name);
	}

	bytecode.m_string_pool = std::move(m_string_pool);
	bytecode.m_imports = std::move(imports).value();
	bytecode.m_global_variables = module.m_globals
		| std::views::filter([](const MidoriIRGlobal& global) { return !global.IsImported(); })
		| std::views::transform([](const MidoriIRGlobal& global) { return global.m_name; })
		| std::ranges::to<std::vector>();
	bytecode.m_exports = m_lowered.m_exports
		| std::views::transform([this](const LoweredExport& exported)
		{
			const size_t global_index = exported.m_slot.has_value() ? static_cast<size_t>(GlobalOperand(exported.m_slot.value())) : 0uz;
			return BytecodeModule::ExportedSymbol(exported.m_name, 0uz, global_index, exported.m_kind, MakeSourceProvenance(exported.m_token));
		})
		| std::ranges::to<std::vector>();
	if (!m_file_name.empty() && !m_source_lines.empty())
	{
		bytecode.m_source_files.emplace(m_file_name, m_source_lines);
	}
	return bytecode;
}

std::optional<BytecodeModule::SourceProvenance> BytecodeBackend::MakeSourceProvenance(const Token& token) const
{
	if (token.m_line <= 0)
	{
		return std::nullopt;
	}

	std::optional<std::string> source_line = std::nullopt;
	if (token.m_line <= static_cast<int>(m_source_lines.size()))
	{
		source_line = m_source_lines[token.m_line - 1];
	}

	std::optional<size_t> caret_length = std::nullopt;
	if (token.m_column.has_value())
	{
		caret_length = std::max(token.m_source_length.value_or(0uz), 1uz);
	}

	return BytecodeModule::SourceProvenance(token.m_line, token.m_column, caret_length, std::move(source_line));
}

CompilerError BytecodeBackend::LimitExceeded(std::string_view message, int line) const
{
	return MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::CodeGeneratorLimitExceeded, message, line, m_file_name, m_source_lines);
}

size_t BytecodeBackend::ProcedureIndex(MidoriIRFunctionId function) const
{
	return m_procedure_indices[function.m_index];
}

int BytecodeBackend::GlobalOperand(MidoriIRGlobalSlot slot) const
{
	return m_global_operands[slot.m_value];
}

std::expected<int, CompilerError> BytecodeBackend::TextConstant(const std::string& text, int line)
{
	const std::unordered_map<std::string, int>::const_iterator existing = m_string_indices.find(text);
	if (existing != m_string_indices.cend())
	{
		return existing->second;
	}
	if (static_cast<int>(m_string_pool.size()) + 1 >= MAX_SIZE_OP_CONSTANT_LONG)
	{
		return std::unexpected(LimitExceeded("Too many text constants", line));
	}

	const int index = static_cast<int>(m_string_pool.size());
	m_string_pool.push_back(text);
	m_string_indices.emplace(text, index);
	return index;
}

// This module's globals are numbered in slot order; an imported one is a
// placeholder the linker replaces with the global of the module defining it.
std::expected<std::vector<BytecodeModule::ImportedSymbol>, CompilerError> BytecodeBackend::AssignGlobals()
{
	const MidoriIRModule& module = m_lowered.m_module;
	std::unordered_map<uint32_t, const Token*> import_tokens;
	for (const LoweredImport& imported : m_lowered.m_imports)
	{
		import_tokens.emplace(imported.m_slot.m_value, &imported.m_token);
	}

	std::vector<BytecodeModule::ImportedSymbol> imports;
	int next_global = 0;
	m_global_operands.reserve(module.m_globals.size());
	for (uint32_t slot = 0u; slot < module.m_globals.size(); slot += 1u)
	{
		const MidoriIRGlobal& global = module.m_globals[slot];
		if (!global.IsImported())
		{
			if (next_global > MAX_VARIABLES)
			{
				return std::unexpected(LimitExceeded(std::format("Too many global variables (max {})", MAX_VARIABLES), 0));
			}
			m_global_operands.push_back(next_global++);
			continue;
		}

		const Token& token = *import_tokens.at(slot);
		if (imports.size() >= static_cast<size_t>(MAX_IMPORT_PLACEHOLDERS))
		{
			return std::unexpected(LimitExceeded(std::format("Too many imports (max {})", MAX_IMPORT_PLACEHOLDERS), token.m_line));
		}
		m_global_operands.push_back(IMPORT_PLACEHOLDER_BASE + static_cast<int>(imports.size()));
		imports.emplace_back(global.m_name, global.m_module, MakeSourceProvenance(token));
	}
	return imports;
}
