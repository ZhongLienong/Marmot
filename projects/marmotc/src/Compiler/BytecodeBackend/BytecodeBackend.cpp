#include "BytecodeBackend.h"
#include "Bytecode/Builtins/BuiltinTable.h"
#include "Bytecode/Format/Format.h"
#include "Compiler/Constant/Constant.h"
#include "Compiler/Lowering/GenericTypes.h"

#include <algorithm>
#include <array>
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

	// Jumps when the comparison fails, taking both operands.
	std::optional<OpCode> CompareAndJumpOpCode(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case LtInt: return OpCode::IF_INTEGER_LESS;
		case LeInt: return OpCode::IF_INTEGER_LESS_EQUAL;
		case GtInt: return OpCode::IF_INTEGER_GREATER;
		case GeInt: return OpCode::IF_INTEGER_GREATER_EQUAL;
		case EqInt: return OpCode::IF_INTEGER_EQUAL;
		case NeInt: return OpCode::IF_INTEGER_NOT_EQUAL;
		case LtFloat: return OpCode::IF_FLOAT_LESS;
		case LeFloat: return OpCode::IF_FLOAT_LESS_EQUAL;
		case GtFloat: return OpCode::IF_FLOAT_GREATER;
		case GeFloat: return OpCode::IF_FLOAT_GREATER_EQUAL;
		case EqFloat: return OpCode::IF_FLOAT_EQUAL;
		case NeFloat: return OpCode::IF_FLOAT_NOT_EQUAL;
		default: return std::nullopt;
		}
	}

	// Jumps when the comparison of two locals fails.
	std::optional<OpCode> CompareLocalsAndJumpOpCode(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case LtInt: return OpCode::IF_LOCAL_LT_LOCAL;
		case GeInt: return OpCode::IF_LOCAL_GE_LOCAL;
		case EqInt: return OpCode::IF_LOCAL_EQ_LOCAL;
		default: return std::nullopt;
		}
	}

	// Operations whose operands may be pushed in either order.
	bool IsCommutative(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case AddInt:
		case MulInt:
		case BitAndInt:
		case BitOrInt:
		case BitXorInt:
		case EqInt:
		case NeInt:
		case EqBool:
		case NeBool:
		case AddByte:
		case MulByte:
		case EqByte:
		case NeByte:
		case AddWord:
		case MulWord:
		case EqWord:
		case NeWord:
			return true;
		default:
			return false;
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

	// The wide form of a local or capture operation, for an index past one byte.
	OpCode WideOpCode(OpCode op)
	{
		switch (op)
		{
		case OpCode::GET_LOCAL: return OpCode::GET_LOCAL_WIDE;
		case OpCode::SET_LOCAL: return OpCode::SET_LOCAL_WIDE;
		case OpCode::GET_CELL: return OpCode::GET_CELL_WIDE;
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

	// A call whose first operand is what it calls, which the VM takes from on
	// top of the arguments: a closure, or a foreign function's name.
	bool CallsClosureOperand(const MidoriIRInstruction& instruction)
	{
		const bool has_no_callee = std::holds_alternative<std::monostate>(instruction.m_immediate);
		return instruction.m_op == MidoriIROp::CallValue || (has_no_callee && (instruction.m_op == MidoriIROp::TailCall || instruction.m_op == MidoriIROp::CallForeign));
	}

	// Whether the instruction leaves its value on the stack.
	bool PushesResult(const MidoriIRInstruction& instruction)
	{
		return !IsMidoriIRTerminator(instruction.m_op) && instruction.m_op != MidoriIROp::GlobalDefine && instruction.m_op != MidoriIROp::GlobalSet && instruction.m_op != MidoriIROp::BindCaptures;
	}

	// The blocks a path from the entry runs, in order. The others hold only
	// code after a call that returns Never, and are not emitted.
	std::vector<bool> ReachableBlocks(const MidoriIRFunction& function)
	{
		std::vector<bool> reachable(function.m_blocks.size(), false);
		std::vector<uint32_t> pending = { MidoriIRFunction::s_entry_block.m_index };
		reachable[MidoriIRFunction::s_entry_block.m_index] = true;
		while (!pending.empty())
		{
			const uint32_t block = pending.back();
			pending.pop_back();
			for (const MidoriIRSuccessor& successor : function.m_blocks[block].m_instructions.back().m_successors)
			{
				if (!reachable[successor.m_block.m_index])
				{
					reachable[successor.m_block.m_index] = true;
					pending.push_back(successor.m_block.m_index);
				}
			}
		}
		return reachable;
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
		std::vector<bool> m_reachable;
		// The block emitted after each, where a jump to it falls through.
		std::vector<std::optional<uint32_t>> m_next_emitted;
		std::vector<Fixup> m_fixups;
		// A comparison a Branch takes straight from the instruction before it
		// becomes one compare-and-jump opcode, by the value it would have made.
		std::vector<std::optional<OpCode>> m_fused_branches;
		// A union's tag that IF_LOCAL_TAG_NOT reads from the union's slot, by
		// the value GetTag would have made.
		std::vector<bool> m_fused_tags;
		int m_slot_count = 0;
		// The instruction emitted after the current one in its block.
		const MidoriIRInstruction* m_next = nullptr;
		// A value just stored and left on the stack too, because the next
		// instruction pushes it first.
		std::optional<MidoriIRValueId> m_kept;

	public:
		FunctionEmitter(BytecodeBackend& backend, const MidoriIRFunction& function)
			: m_backend(backend),
			m_function(function),
			m_storage(function.m_values.size(), Storage::Slot),
			m_slots(function.m_values.size(), -1),
			m_definitions(function.m_values.size(), nullptr),
			m_block_offsets(function.m_blocks.size(), -1),
			m_reachable(ReachableBlocks(function)),
			m_next_emitted(function.m_blocks.size()),
			m_fused_branches(function.m_values.size()),
			m_fused_tags(function.m_values.size(), false)
		{
			std::optional<uint32_t> previous;
			for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
			{
				if (!m_reachable[block])
				{
					continue;
				}
				if (previous.has_value())
				{
					m_next_emitted[previous.value()] = block;
				}
				previous = block;
			}
		}

		std::expected<BytecodeStream, CompilerError> Emit() &&
		{
			AssignStorage();
			ScheduleStack();
			AssignSlots();
			FuseBranches();
			if (m_slot_count - 1 > MAX_VARIABLES)
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many variables (max {})", MAX_VARIABLES), FirstLine()));
			}

			const int arity = static_cast<int>(m_function.Block(MidoriIRFunction::s_entry_block).m_parameters.size());
			for (int slot = arity; slot < m_slot_count; slot += BYTE_MASK)
			{
				EmitByte(OpCode::PUSH_PLACEHOLDER, FirstLine());
				EmitOperand(std::min(m_slot_count - slot, static_cast<int>(BYTE_MASK)), FirstLine());
			}

			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_reachable[block])
				{
					continue;
				}
				m_block_offsets[block] = m_stream.GetByteCodeSize();
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				for (size_t position = 0u; position < instructions.size(); position += 1u)
				{
					m_next = position + 1u < instructions.size() ? &instructions[position + 1u] : nullptr;
					const std::optional<MidoriIRValueId> kept = m_kept;
					const Emitted emitted = EmitInstruction(MidoriIRBlockId{ block }, instructions[position]);
					if (!emitted.has_value())
					{
						return std::unexpected(emitted.error());
					}
					if (kept.has_value() && m_kept == kept)
					{
						throw std::logic_error(std::format("{} did not take the value left for it on the stack", GetMidoriIROpInfo(instructions[position].m_op).m_name));
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
			// A value left on the stack must be taken first, and either
			// operand of a commutative operation may be.
			const bool takes_second_from_stack = IsCommutative(instruction.m_op)
				&& m_storage[instruction.m_operands[1u].m_index] == Storage::Stack
				&& m_storage[instruction.m_operands[0u].m_index] != Storage::Stack;
			if (takes_second_from_stack)
			{
				return { instruction.m_operands[1u], instruction.m_operands[0u] };
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
				if (!m_reachable[block])
				{
					continue;
				}
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
			KeepStepsInPlace();
		}

		// A loop's `p + c` passed straight back as p is kept in a slot rather
		// than on the stack: p dies there, so the sum takes p's slot and is
		// one STEP_LOCAL, and the jump moves nothing.
		void KeepStepsInPlace()
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				if (!m_reachable[block] || instructions.back().m_op != MidoriIROp::Jump)
				{
					continue;
				}
				const MidoriIRSuccessor& target = instructions.back().m_successors.front();
				const std::vector<MidoriIRValueId>& parameters = TargetOf(target).m_parameters;
				for (size_t index = 0u; index < parameters.size(); index += 1u)
				{
					const MidoriIRValueId argument = target.m_arguments[index];
					const MidoriIRInstruction* definition = m_definitions[argument.m_index];
					if (m_storage[argument.m_index] != Storage::Stack || definition == nullptr)
					{
						continue;
					}
					const std::optional<std::pair<MidoriIRValueId, int64_t>> step = LocalStep(*definition);
					if (step.has_value() && step->first == parameters[index] && !IsUsedAfter(block, *definition, step->first))
					{
						m_storage[argument.m_index] = Storage::Slot;
					}
				}
			}
		}

		// Whether an instruction after `definition` in the block reads `value`.
		bool IsUsedAfter(uint32_t block, const MidoriIRInstruction& definition, MidoriIRValueId value) const
		{
			const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
			const std::vector<MidoriIRInstruction>::const_iterator after = std::ranges::find_if(instructions, [&definition](const MidoriIRInstruction& instruction) { return &instruction == &definition; }) + 1;
			bool used = false;
			for (const MidoriIRInstruction& instruction : std::ranges::subrange(after, instructions.end()))
			{
				ForEachUse(instruction, [&](MidoriIRValueId use) { used = used || use == value; });
			}
			return used;
		}

		// A value can stay on the stack only if, when its use runs, it is where
		// that use takes it: the stack values an instruction takes must come
		// first among its operands, and be the top of the stack in order. One
		// that is not moves to a slot; it was stored when it was made, so it
		// leaves the stack it was on, and the use is checked again. A Text
		// constant is a new text each time it is loaded, so one used once is
		// loaded at its use instead.
		void ScheduleStack()
		{
			for (uint32_t block_index = 0u; block_index < m_function.m_blocks.size(); block_index += 1u)
			{
				if (!m_reachable[block_index])
				{
					continue;
				}
				std::vector<MidoriIRValueId> pending;
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block_index].m_instructions)
				{
					std::vector<MidoriIRValueId> sequence = PushSequence(instruction);
					std::vector<MidoriIRValueId> taken = StackValues(sequence);
					while (!IsTakenFromTop(sequence, taken, pending))
					{
						for (const MidoriIRValueId value : taken)
						{
							const MidoriIRInstruction* definition = m_definitions[value.m_index];
							m_storage[value.m_index] = definition->m_op == MidoriIROp::Const ? Storage::Rematerialized : Storage::Slot;
							std::erase(pending, value);
						}
						sequence = PushSequence(instruction);
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

		// A local an opcode names in one byte.
		bool IsByteLocal(MidoriIRValueId value) const
		{
			return m_storage[value.m_index] == Storage::Slot && m_slots[value.m_index] <= MAX_LOCAL_VARIABLES;
		}

		// An Int constant an opcode carries in one signed byte.
		std::optional<int8_t> ByteConstant(MidoriIRValueId value) const
		{
			const MidoriIRInstruction* definition = m_definitions[value.m_index];
			if (m_storage[value.m_index] != Storage::Rematerialized || definition == nullptr || !std::holds_alternative<int64_t>(definition->m_immediate))
			{
				return std::nullopt;
			}
			const int64_t constant = std::get<int64_t>(definition->m_immediate);
			return constant >= INT8_MIN && constant <= INT8_MAX ? std::optional<int8_t>(static_cast<int8_t>(constant)) : std::nullopt;
		}

		void FuseBranches()
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				if (!m_reachable[block] || instructions.size() < 2u || instructions.back().m_op != MidoriIROp::Branch)
				{
					continue;
				}
				const MidoriIRValueId condition = instructions.back().m_operands.front();
				const MidoriIRInstruction& comparison = instructions[instructions.size() - 2u];
				const std::optional<OpCode> jump = CompareAndJumpOpCode(comparison.m_op);
				if (comparison.m_result != condition || m_storage[condition.m_index] != Storage::Stack || !jump.has_value())
				{
					continue;
				}

				const MidoriIRValueId left = comparison.m_operands[0u];
				const MidoriIRValueId right = comparison.m_operands[1u];
				const std::optional<MidoriIRValueId> tag = FusableTag(instructions, comparison);
				if (tag.has_value())
				{
					m_fused_branches[condition.m_index] = OpCode::IF_LOCAL_TAG_NOT;
					m_fused_tags[tag->m_index] = true;
				}
				else if (comparison.m_op == MidoriIROp::LeInt && IsByteLocal(left) && ByteConstant(right).has_value())
				{
					m_fused_branches[condition.m_index] = OpCode::IF_LOCAL_LE_INT;
				}
				else if (comparison.m_op == MidoriIROp::LtInt && IsByteLocal(left) && ByteConstant(right).has_value())
				{
					m_fused_branches[condition.m_index] = OpCode::IF_LOCAL_LT_INT;
				}
				else if (CompareLocalsAndJumpOpCode(comparison.m_op).has_value() && IsByteLocal(left) && IsByteLocal(right))
				{
					m_fused_branches[condition.m_index] = CompareLocalsAndJumpOpCode(comparison.m_op);
				}
				else
				{
					m_fused_branches[condition.m_index] = jump;
				}
			}
		}

		// The tag in `tag == k`, when the union it is read from is a local and
		// k fits a byte, so the branch reads the tag from the union's slot.
		// Nothing emitted may lie between the GetTag and the comparison,
		// since another value may take the union's slot once the tag is read.
		std::optional<MidoriIRValueId> FusableTag(const std::vector<MidoriIRInstruction>& instructions, const MidoriIRInstruction& comparison) const
		{
			if (comparison.m_op != MidoriIROp::EqInt)
			{
				return std::nullopt;
			}
			const bool is_tag_first = TagConstant(comparison.m_operands[1u]).has_value();
			const MidoriIRValueId tag = comparison.m_operands[is_tag_first ? 0u : 1u];
			const MidoriIRInstruction* get_tag = m_definitions[tag.m_index];
			if (!TagConstant(comparison.m_operands[is_tag_first ? 1u : 0u]).has_value() || get_tag == nullptr || get_tag->m_op != MidoriIROp::GetTag || m_storage[tag.m_index] != Storage::Stack || !IsByteLocal(get_tag->m_operands.front()))
			{
				return std::nullopt;
			}
			const auto position = [&instructions](const MidoriIRInstruction& target)
			{
				return std::ranges::find_if(instructions, [&target](const MidoriIRInstruction& instruction) { return &instruction == &target; });
			};
			const bool emits_nothing_between = std::ranges::all_of(std::ranges::subrange(position(*get_tag) + 1, position(comparison)), [this](const MidoriIRInstruction& instruction)
			{
				const Storage storage = m_storage[instruction.m_result->m_index];
				return instruction.m_op == MidoriIROp::Const && (storage == Storage::Rematerialized || storage == Storage::Discarded);
			});
			return emits_nothing_between ? std::optional(tag) : std::nullopt;
		}

		std::optional<uint8_t> TagConstant(MidoriIRValueId value) const
		{
			const MidoriIRInstruction* definition = m_definitions[value.m_index];
			if (m_storage[value.m_index] != Storage::Rematerialized || definition == nullptr || !std::holds_alternative<int64_t>(definition->m_immediate))
			{
				return std::nullopt;
			}
			const int64_t constant = std::get<int64_t>(definition->m_immediate);
			return constant >= 0 && constant <= MAX_UNION_TAG ? std::optional(static_cast<uint8_t>(constant)) : std::nullopt;
		}

		// Two values in slots share one when neither is live where the other
		// is defined. A block's parameters are defined where it starts, so
		// they take no slot a value live into the block holds. Parameters
		// are placed first, and a value takes the slot of what it is moved
		// to or from when it can: a jump that passes a value in the slot of
		// the parameter it becomes moves nothing, and a sum stored where its
		// operand was is one STEP_LOCAL.
		void AssignSlots()
		{
			const std::vector<std::vector<uint32_t>> interference = Interference();
			const std::vector<std::vector<uint32_t>> hints = SlotHints();
			const std::vector<MidoriIRValueId>& parameters = m_function.Block(MidoriIRFunction::s_entry_block).m_parameters;
			for (const MidoriIRValueId parameter : parameters)
			{
				m_slots[parameter.m_index] = m_slot_count++;
			}

			std::vector<uint32_t> order;
			for (uint32_t block = 1u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (m_reachable[block])
				{
					std::ranges::transform(m_function.m_blocks[block].m_parameters, std::back_inserter(order), [](MidoriIRValueId parameter) { return parameter.m_index; });
				}
			}
			std::ranges::copy(std::views::iota(0u, static_cast<uint32_t>(m_slots.size())), std::back_inserter(order));

			for (const uint32_t value : order)
			{
				if (m_storage[value] != Storage::Slot || m_slots[value] >= 0)
				{
					continue;
				}
				std::vector<bool> taken(static_cast<size_t>(m_slot_count) + 1u, false);
				for (const uint32_t neighbour : interference[value])
				{
					if (m_slots[neighbour] >= 0)
					{
						taken[static_cast<size_t>(m_slots[neighbour])] = true;
					}
				}
				const std::vector<uint32_t>::const_iterator hinted = std::ranges::find_if(hints[value], [&](uint32_t partner)
				{
					return m_slots[partner] >= 0 && !taken[static_cast<size_t>(m_slots[partner])];
				});
				const int slot = hinted != hints[value].end() ? m_slots[*hinted] : static_cast<int>(std::distance(taken.begin(), std::ranges::find(taken, false)));
				m_slots[value] = slot;
				m_slot_count = std::max(m_slot_count, slot + 1);
			}
		}

		// The values each would do best to share a slot with: a parameter
		// and what edges pass it, a sum of a local and a small constant and
		// that local, an array appended to and the array it becomes.
		std::vector<std::vector<uint32_t>> SlotHints() const
		{
			std::vector<std::vector<uint32_t>> hints(m_function.m_values.size());
			const auto pair = [&](MidoriIRValueId left, MidoriIRValueId right)
			{
				if (left != right && IsInSlot(left) && IsInSlot(right))
				{
					hints[left.m_index].push_back(right.m_index);
					hints[right.m_index].push_back(left.m_index);
				}
			};
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_reachable[block])
				{
					continue;
				}
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					const std::optional<std::pair<MidoriIRValueId, int64_t>> step = LocalStep(instruction);
					if (step.has_value())
					{
						pair(instruction.m_result.value(), step->first);
					}
					if (instruction.m_op == MidoriIROp::ArrayAppend)
					{
						pair(instruction.m_result.value(), instruction.m_operands.front());
					}
					for (const MidoriIRSuccessor& successor : instruction.m_successors)
					{
						const std::vector<MidoriIRValueId>& target = TargetOf(successor).m_parameters;
						for (size_t index = 0u; index < target.size(); index += 1u)
						{
							pair(successor.m_arguments[index], target[index]);
						}
					}
				}
			}
			return hints;
		}

		// A sum or difference of a value and a small constant, as the value
		// and what it adds.
		std::optional<std::pair<MidoriIRValueId, int64_t>> LocalStep(const MidoriIRInstruction& instruction) const
		{
			if (instruction.m_op != MidoriIROp::AddInt && instruction.m_op != MidoriIROp::SubInt)
			{
				return std::nullopt;
			}
			const MidoriIRValueId left = instruction.m_operands[0u];
			const MidoriIRValueId right = instruction.m_operands[1u];
			const std::optional<int8_t> right_constant = ByteConstant(right);
			if (right_constant.has_value() && m_storage[left.m_index] != Storage::Rematerialized)
			{
				const int64_t constant = right_constant.value();
				return std::pair{ left, instruction.m_op == MidoriIROp::AddInt ? constant : -constant };
			}
			const std::optional<int8_t> left_constant = ByteConstant(left);
			if (instruction.m_op == MidoriIROp::AddInt && left_constant.has_value() && m_storage[right.m_index] != Storage::Rematerialized)
			{
				return std::pair{ right, static_cast<int64_t>(left_constant.value()) };
			}
			return std::nullopt;
		}

		bool IsInSlot(MidoriIRValueId value) const
		{
			return m_storage[value.m_index] == Storage::Slot;
		}

		// What an instruction reads: its operands and its successors' arguments.
		template<typename Visit>
		void ForEachUse(const MidoriIRInstruction& instruction, const Visit& visit) const
		{
			std::ranges::for_each(instruction.m_operands, visit);
			for (const MidoriIRSuccessor& successor : instruction.m_successors)
			{
				std::ranges::for_each(successor.m_arguments, visit);
			}
		}

		std::vector<bool> LiveOut(uint32_t block, const std::vector<std::vector<bool>>& live_in) const
		{
			std::vector<bool> live(m_function.m_values.size(), false);
			for (const MidoriIRSuccessor& successor : m_function.m_blocks[block].m_instructions.back().m_successors)
			{
				const std::vector<bool>& successor_live = live_in[successor.m_block.m_index];
				for (size_t value = 0u; value < live.size(); value += 1u)
				{
					live[value] = live[value] || successor_live[value];
				}
			}
			return live;
		}

		// Walks a block backward from what is live out of it, telling `define`
		// each value in a slot the block defines, with what is live after it.
		template<typename Define>
		std::vector<bool> WalkBackward(uint32_t block, std::vector<bool> live, const Define& define) const
		{
			const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
			for (const MidoriIRInstruction& instruction : instructions | std::views::reverse)
			{
				if (instruction.m_result.has_value() && IsInSlot(instruction.m_result.value()))
				{
					define(instruction.m_result.value(), live);
					live[instruction.m_result->m_index] = false;
				}
				ForEachUse(instruction, [&](MidoriIRValueId value)
				{
					if (IsInSlot(value))
					{
						live[value.m_index] = true;
					}
				});
			}
			return live;
		}

		std::vector<std::vector<uint32_t>> Interference() const
		{
			const size_t block_count = m_function.m_blocks.size();
			std::vector<std::vector<bool>> live_in(block_count, std::vector<bool>(m_function.m_values.size(), false));
			const auto no_definition = [](MidoriIRValueId, const std::vector<bool>&) {};
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (uint32_t block = static_cast<uint32_t>(block_count); block-- > 0u;)
				{
					if (!m_reachable[block])
					{
						continue;
					}
					std::vector<bool> live = WalkBackward(block, LiveOut(block, live_in), no_definition);
					for (const MidoriIRValueId parameter : m_function.m_blocks[block].m_parameters)
					{
						live[parameter.m_index] = false;
					}
					if (live != live_in[block])
					{
						live_in[block] = std::move(live);
						changed = true;
					}
				}
			}

			std::vector<std::vector<uint32_t>> interference(m_function.m_values.size());
			const auto interfere = [&interference](uint32_t value, const std::vector<bool>& live)
			{
				for (uint32_t other = 0u; other < live.size(); other += 1u)
				{
					if (live[other] && other != value)
					{
						interference[value].push_back(other);
						interference[other].push_back(value);
					}
				}
			};
			for (uint32_t block = 0u; block < block_count; block += 1u)
			{
				if (!m_reachable[block])
				{
					continue;
				}
				std::vector<bool> live = WalkBackward(block, LiveOut(block, live_in), [&](MidoriIRValueId value, const std::vector<bool>& live_after)
				{
					interfere(value.m_index, live_after);
				});
				const std::vector<MidoriIRValueId>& parameters = m_function.m_blocks[block].m_parameters;
				for (const MidoriIRValueId parameter : parameters)
				{
					live[parameter.m_index] = true;
				}
				for (const MidoriIRValueId parameter : parameters)
				{
					interfere(parameter.m_index, live);
				}
			}
			return interference;
		}

		void EmitByte(OpCode byte, int line)
		{
			m_stream.AddByteCode(byte, line);
		}

		void EmitOperand(int value, int line)
		{
			m_stream.AddByteCode(static_cast<OpCode>(value & BYTE_MASK), line);
		}

		// An index of a local or a capture, in one byte or, wide, in two, high first.
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

		// A global's index is always two bytes, high first: the linker adds the
		// module's first global to it, which may pass 255.
		void EmitGlobal(OpCode op, int index, int line)
		{
			EmitByte(op, line);
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
			if (m_kept == value)
			{
				m_kept.reset();
				return {};
			}
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
			for (size_t index = 0u; index < values.size(); index += 1u)
			{
				const bool is_kept = index == 0u && m_kept == values[index];
				if (!is_kept && index + 1u < values.size() && IsByteLocal(values[index]) && IsByteLocal(values[index + 1u]))
				{
					EmitByte(OpCode::GET_LOCAL2, line);
					EmitOperand(m_slots[values[index].m_index], line);
					EmitOperand(m_slots[values[index + 1u].m_index], line);
					index += 1u;
					continue;
				}
				const Emitted loaded = Load(values[index], line);
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
				StoreInSlot(value, line);
				return;
			case Storage::Rematerialized:
			case Storage::Discarded:
				EmitByte(OpCode::POP, line);
				return;
			}
		}

		void StoreInSlot(MidoriIRValueId value, int line)
		{
			const int slot = m_slots[value.m_index];
			if (IsLoadedNext(value) || slot > MAX_LOCAL_VARIABLES)
			{
				EmitVariable(OpCode::SET_LOCAL, slot, line);
				KeepOrPop(value, line);
				return;
			}
			EmitByte(OpCode::STORE_LOCAL, line);
			EmitOperand(slot, line);
		}

		bool IsLoadedNext(MidoriIRValueId value) const
		{
			return m_next != nullptr && FirstLoad(*m_next) == value;
		}

		// A value in its slot that is also on top of the stack stays there
		// when the next instruction would push it first.
		void KeepOrPop(MidoriIRValueId value, int line)
		{
			if (IsLoadedNext(value))
			{
				m_kept = value;
				return;
			}
			EmitByte(OpCode::POP, line);
		}

		// The value an instruction's emission loads first, as EmitInstruction
		// and EmitTerminator do it; nothing when it reads slots directly or
		// loads nothing.
		std::optional<MidoriIRValueId> FirstLoad(const MidoriIRInstruction& instruction) const
		{
			const auto first = [](const std::vector<MidoriIRValueId>& values) { return values.empty() ? std::nullopt : std::optional(values.front()); };
			switch (instruction.m_op)
			{
			case MidoriIROp::Const:
			case MidoriIROp::Unreachable:
				return std::nullopt;
			case MidoriIROp::Jump:
			{
				const std::vector<std::pair<MidoriIRValueId, MidoriIRValueId>> moves = Moves(instruction.m_successors.front());
				return moves.empty() ? std::nullopt : std::optional(moves.front().first);
			}
			case MidoriIROp::Branch:
				return m_fused_branches[instruction.m_operands.front().m_index].has_value() ? std::nullopt : std::optional(instruction.m_operands.front());
			case MidoriIROp::Return:
				return instruction.m_operands.front();
			case MidoriIROp::TailCall:
				return first(PushSequence(instruction));
			default:
				break;
			}
			const uint32_t result = instruction.m_result->m_index;
			if (ReadsLocalsDirectly(m_fused_branches[result]) || m_fused_tags[result] || IsLocalStep(instruction) || ReadsAndWritesLocals(instruction))
			{
				return std::nullopt;
			}
			return first(PushSequence(instruction));
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

			const std::optional<OpCode> fused = instruction.m_result.has_value() ? m_fused_branches[instruction.m_result->m_index] : std::nullopt;
			if (ReadsLocalsDirectly(fused) || (instruction.m_result.has_value() && m_fused_tags[instruction.m_result->m_index]))
			{
				return {};
			}
			if (fused.has_value())
			{
				return LoadAll(PushSequence(instruction), line);
			}

			if (EmitLocalStep(instruction, line))
			{
				return {};
			}

			if (ReadsAndWritesLocals(instruction))
			{
				EmitLocalsOperation(instruction, line);
				return {};
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

		// A fused branch that reads its operands from their slots, so the
		// comparison it replaces loads nothing.
		static bool ReadsLocalsDirectly(std::optional<OpCode> fused)
		{
			return fused == OpCode::IF_LOCAL_LE_INT || fused == OpCode::IF_LOCAL_LT_INT || fused == OpCode::IF_LOCAL_GE_LOCAL || fused == OpCode::IF_LOCAL_LT_LOCAL || fused == OpCode::IF_LOCAL_EQ_LOCAL || fused == OpCode::IF_LOCAL_TAG_NOT;
		}

		// An operation on locals whose result is stored in a local, which one
		// opcode does without the stack: a union's field, an array's element,
		// a union of two fields, and an append that grows the array in the slot
		// it is stored back in.
		bool ReadsAndWritesLocals(const MidoriIRInstruction& instruction) const
		{
			const std::vector<MidoriIRValueId>& operands = instruction.m_operands;
			switch (instruction.m_op)
			{
			case MidoriIROp::UnionField:
				return IsByteLocal(operands.front()) && IsByteLocal(instruction.m_result.value());
			case MidoriIROp::ArrayGet:
				return IsByteLocal(operands[0u]) && IsByteLocal(operands[1u]) && IsByteLocal(instruction.m_result.value());
			case MidoriIROp::ArrayAppend:
				return IsByteLocal(operands[0u]) && IsByteLocal(operands[1u]) && IsInSlot(instruction.m_result.value()) && m_slots[instruction.m_result->m_index] == m_slots[operands[0u].m_index];
			case MidoriIROp::MakeUnion:
				return operands.size() == 2u && std::get<MidoriIRTag>(instruction.m_immediate).m_value <= MAX_UNION_TAG && IsByteLocal(operands[0u]) && IsByteLocal(operands[1u]) && IsByteLocal(instruction.m_result.value());
			default:
				return false;
			}
		}

		void EmitLocalsOperation(const MidoriIRInstruction& instruction, int line)
		{
			const std::vector<MidoriIRValueId>& operands = instruction.m_operands;
			switch (instruction.m_op)
			{
			case MidoriIROp::UnionField:
				EmitByte(OpCode::LOCAL_UNION_FIELD, line);
				EmitOperand(m_slots[operands.front().m_index], line);
				EmitOperand(static_cast<int>(std::get<MidoriIRUnionField>(instruction.m_immediate).m_index), line);
				EmitOperand(m_slots[instruction.m_result->m_index], line);
				return;
			case MidoriIROp::ArrayGet:
				EmitByte(OpCode::LOCAL_ARRAY_GET, line);
				EmitOperand(m_slots[operands[0u].m_index], line);
				EmitOperand(m_slots[operands[1u].m_index], line);
				EmitOperand(m_slots[instruction.m_result->m_index], line);
				return;
			case MidoriIROp::ArrayAppend:
				EmitByte(OpCode::APPEND_LOCAL, line);
				EmitOperand(m_slots[operands[0u].m_index], line);
				EmitOperand(m_slots[operands[1u].m_index], line);
				return;
			case MidoriIROp::MakeUnion:
				EmitByte(OpCode::LOCAL_UNION2, line);
				EmitOperand(std::get<MidoriIRTag>(instruction.m_immediate).m_value, line);
				EmitOperand(m_slots[operands[0u].m_index], line);
				EmitOperand(m_slots[operands[1u].m_index], line);
				EmitOperand(m_slots[instruction.m_result->m_index], line);
				return;
			default:
				throw std::logic_error(std::format("{} does not read and write locals", GetMidoriIROpInfo(instruction.m_op).m_name));
			}
		}

		// A local plus or minus a small constant. Stored back in the local's
		// own slot, which it may share only when the local dies here, it is
		// STEP_LOCAL, or ADD_LOCAL_INT when the next instruction takes it from
		// the stack; otherwise PUSH_LOCAL_SUB_INT pushes it.
		bool IsLocalStep(const MidoriIRInstruction& instruction) const
		{
			const std::optional<std::pair<MidoriIRValueId, int64_t>> step = LocalStep(instruction);
			if (!step.has_value() || !IsByteLocal(step->first))
			{
				return false;
			}
			const auto [local, delta] = step.value();
			const MidoriIRValueId result = instruction.m_result.value();
			const bool is_in_place = IsInSlot(result) && m_slots[result.m_index] == m_slots[local.m_index] && delta >= INT8_MIN && delta <= INT8_MAX;
			return is_in_place || (-delta >= INT8_MIN && -delta <= INT8_MAX);
		}

		bool EmitLocalStep(const MidoriIRInstruction& instruction, int line)
		{
			if (!IsLocalStep(instruction))
			{
				return false;
			}
			const auto [local, delta] = LocalStep(instruction).value();
			const MidoriIRValueId result = instruction.m_result.value();
			const int slot = m_slots[local.m_index];
			if (IsInSlot(result) && m_slots[result.m_index] == slot && delta >= INT8_MIN && delta <= INT8_MAX)
			{
				const bool is_loaded_next = IsLoadedNext(result);
				EmitByte(is_loaded_next ? OpCode::ADD_LOCAL_INT : OpCode::STEP_LOCAL, line);
				EmitOperand(slot, line);
				EmitOperand(static_cast<uint8_t>(static_cast<int8_t>(delta)), line);
				if (is_loaded_next)
				{
					m_kept = result;
				}
				return true;
			}
			EmitByte(OpCode::PUSH_LOCAL_SUB_INT, line);
			EmitOperand(slot, line);
			EmitOperand(static_cast<uint8_t>(static_cast<int8_t>(-delta)), line);
			Store(result, line);
			return true;
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
			if (procedure > static_cast<size_t>(UINT16_MAX))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many functions (max {})", UINT16_MAX + 1), line));
			}
			return static_cast<int>(procedure);
		}

		// A procedure is two bytes, low first: the linker adds the module's
		// first procedure to it.
		void EmitProcedure(OpCode op, int procedure, int line)
		{
			EmitByte(op, line);
			EmitOperand(procedure, line);
			EmitOperand(procedure >> SHIFT_8_BITS, line);
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

		static bool IsText(const TypeRef& type)
		{
			return GenericTypes::RepresentationOf(type)->IsType<MidoriType::TextType>();
		}

		Emitted CheckByte(size_t count, std::string_view what, int line) const
		{
			if (count > static_cast<size_t>(BYTE_MASK))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("{} (max {})", what, static_cast<int>(BYTE_MASK)), line));
			}
			return {};
		}

		void EmitSmallInt(uint32_t value, int line)
		{
			const std::optional<OpCode> small = SmallIntOpCode(static_cast<int64_t>(value));
			if (small.has_value())
			{
				EmitByte(small.value(), line);
				return;
			}
			EmitByte(OpCode::INTEGER_CONSTANT, line);
			EmitEightBytes(value, line);
		}

		// A tuple's or an array's element count, in three bytes, low first.
		Emitted EmitCount(OpCode op, size_t count, int line)
		{
			if (count > static_cast<size_t>(MAX_ARRAY_SIZE))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many elements (max {})", MAX_ARRAY_SIZE + 1), line));
			}
			EmitByte(op, line);
			EmitOperand(static_cast<int>(count), line);
			EmitOperand(static_cast<int>(count >> SHIFT_8_BITS), line);
			EmitOperand(static_cast<int>(count >> SHIFT_16_BITS), line);
			return {};
		}

		// A function with no captures is one shared value. A closure's
		// captures follow its operands, and the ones BindCaptures fills are
		// held by Unit until it does.
		Emitted EmitMakeClosure(const MidoriIRInstruction& instruction, int line)
		{
			const MidoriIRFunctionId function = std::get<MidoriIRFunctionId>(instruction.m_immediate);
			const size_t capture_count = m_backend.CaptureCount(function);
			if (capture_count > static_cast<size_t>(MAX_CAPTURED_COUNT))
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Too many captured variables (max {})", MAX_CAPTURED_COUNT + 1), line));
			}
			return Procedure(function, line)
				.transform([&](int procedure)
				{
					if (capture_count == 0u)
					{
						EmitProcedure(OpCode::MAKE_FUNCTION_WIDE, procedure, line);
						return;
					}
					for (size_t late = instruction.m_operands.size(); late < capture_count; late += 1u)
					{
						EmitByte(OpCode::OP_UNIT, line);
					}
					EmitProcedure(OpCode::MAKE_CLOSURE_OF, procedure, line);
					EmitOperand(static_cast<int>(capture_count), line);
				});
		}

		// A union with no fields is one shared value per tag.
		Emitted EmitMakeUnion(const MidoriIRInstruction& instruction, int line)
		{
			const int tag = std::get<MidoriIRTag>(instruction.m_immediate).m_value;
			if (tag > MAX_UNION_TAG)
			{
				return std::unexpected(m_backend.LimitExceeded(std::format("Union tag too large (max {})", MAX_UNION_TAG + 1), line));
			}
			const size_t field_count = instruction.m_operands.size();
			return CheckByte(field_count, "Too many union fields", line)
				.transform([&]()
				{
					if (field_count == 0u)
					{
						EmitByte(OpCode::LOAD_EMPTY_UNION, line);
						EmitOperand(tag, line);
						return;
					}
					EmitByte(OpCode::CONSTRUCT_UNION, line);
					EmitOperand(static_cast<int>(field_count), line);
					EmitOperand(tag, line);
				});
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
				EmitByte(IsText(instruction.m_type) ? OpCode::CONCAT_TEXT : OpCode::CONCAT_ARRAY, line);
				return {};
			case MidoriIROp::Extend:
				EmitByte(IsText(instruction.m_type) ? OpCode::EXTEND_TEXT : OpCode::EXTEND_ARRAY, line);
				return {};
			case MidoriIROp::ArrayAppend:
				EmitByte(OpCode::ADD_BACK_ARRAY, line);
				return {};
			case MidoriIROp::Call:
				return CheckArity(operand_count, line)
					.and_then([&]() { return Procedure(std::get<MidoriIRFunctionId>(instruction.m_immediate), line); })
					.transform([&](int procedure)
					{
						EmitProcedure(OpCode::CALL_PROC_WIDE, procedure, line);
						EmitOperand(static_cast<int>(operand_count), line);
					});
			case MidoriIROp::CallGlobal:
				return CheckArity(operand_count, line)
					.transform([&]()
					{
						EmitGlobal(OpCode::CALL_GLOBAL_WIDE, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
						EmitOperand(static_cast<int>(operand_count), line);
					});
			case MidoriIROp::CallValue:
				return CheckArity(operand_count - 1u, line)
					.and_then([&]() { return EmitCallOf(OpCode::CALL, OpCode::CALL_0, operand_count - 1u, line); });
			case MidoriIROp::CallForeign:
			{
				// A builtin is called by its index; any other foreign function
				// by the name its first operand holds.
				const bool is_builtin = std::holds_alternative<MidoriIRForeign>(instruction.m_immediate);
				const size_t arity = is_builtin ? operand_count : operand_count - 1u;
				return CheckArity(arity, line)
					.transform([&]()
					{
						if (is_builtin)
						{
							EmitByte(OpCode::CALL_FOREIGN_INDEXED, line);
							EmitOperand(static_cast<int>(MarmotBuiltins::FindIndex(std::get<MidoriIRForeign>(instruction.m_immediate).m_name).value()), line);
						}
						else
						{
							EmitByte(OpCode::CALL_FOREIGN, line);
						}
						EmitOperand(static_cast<int>(arity), line);
						EmitOperand(ForeignReturnTag(instruction.m_type), line);
					});
			}
			case MidoriIROp::MakeClosure:
				return EmitMakeClosure(instruction, line);
			case MidoriIROp::BindCaptures:
				EmitByte(OpCode::SET_CAPTURE, line);
				EmitOperand(static_cast<int>(std::get<MidoriIRIndex>(instruction.m_immediate).m_value), line);
				return {};
			case MidoriIROp::GetCapture:
				EmitVariable(OpCode::GET_CELL, static_cast<int>(std::get<MidoriIRIndex>(instruction.m_immediate).m_value), line);
				return {};
			case MidoriIROp::GlobalDefine:
				EmitGlobal(OpCode::DEFINE_GLOBAL_WIDE, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				return {};
			case MidoriIROp::GlobalGet:
				EmitGlobal(OpCode::GET_GLOBAL_WIDE, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				return {};
			case MidoriIROp::GlobalSet:
				EmitGlobal(OpCode::SET_GLOBAL_WIDE, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
				EmitByte(OpCode::POP, line);
				return {};
			case MidoriIROp::MakeTuple:
				return EmitCount(OpCode::CREATE_TUPLE, operand_count, line);
			case MidoriIROp::MakeArray:
				return EmitCount(OpCode::CREATE_ARRAY, operand_count, line);
			case MidoriIROp::TupleGet:
				EmitSmallInt(std::get<MidoriIRIndex>(instruction.m_immediate).m_value, line);
				EmitByte(OpCode::GET_TUPLE, line);
				return {};
			case MidoriIROp::ArrayGet:
				EmitByte(OpCode::GET_ARRAY, line);
				return {};
			case MidoriIROp::ArrayLength:
				EmitByte(OpCode::GET_ARRAY_LENGTH, line);
				return {};
			case MidoriIROp::Construct:
				return CheckByte(operand_count, "Too many struct members", line)
					.transform([&]()
					{
						EmitByte(OpCode::CONSTRUCT_STRUCT, line);
						EmitOperand(static_cast<int>(operand_count), line);
					});
			case MidoriIROp::GetMember:
				EmitByte(OpCode::GET_MEMBER, line);
				EmitOperand(static_cast<int>(std::get<MidoriIRIndex>(instruction.m_immediate).m_value), line);
				return {};
			case MidoriIROp::MakeUnion:
				return EmitMakeUnion(instruction, line);
			case MidoriIROp::GetTag:
				EmitByte(OpCode::GET_TAG, line);
				return {};
			case MidoriIROp::UnionField:
				EmitByte(OpCode::GET_UNION_FIELD, line);
				EmitOperand(static_cast<int>(std::get<MidoriIRUnionField>(instruction.m_immediate).m_index), line);
				return {};
			case MidoriIROp::MakeRange:
				EmitByte(instruction.m_type->GetType<MidoriType::RangeType>().m_element_type->IsType<MidoriType::FloatType>() ? OpCode::CREATE_FLOAT_RANGE : OpCode::CREATE_INT_RANGE, line);
				return {};
			case MidoriIROp::RangeStart:
				EmitByte(OpCode::GET_RANGE_START, line);
				return {};
			case MidoriIROp::RangeEnd:
				EmitByte(OpCode::GET_RANGE_END, line);
				return {};
			case MidoriIROp::RangeStep:
				EmitByte(OpCode::GET_RANGE_STEP, line);
				return {};
			case MidoriIROp::CellNew:
				EmitByte(OpCode::MAKE_CELL, line);
				return {};
			case MidoriIROp::CellRead:
				EmitByte(OpCode::READ_CELL, line);
				return {};
			case MidoriIROp::CellWrite:
				EmitByte(OpCode::WRITE_CELL, line);
				return {};
			case MidoriIROp::Spawn:
				return CheckArity(operand_count - 1u, line)
					.transform([&]()
					{
						EmitByte(OpCode::SPAWN_WORKER, line);
						EmitOperand(static_cast<int>(operand_count - 1u), line);
					});
			case MidoriIROp::Join:
			{
				const MidoriIRJoinTags& tags = std::get<MidoriIRJoinTags>(instruction.m_immediate);
				if (std::ranges::any_of(std::array<int, 4u>{ tags.m_ok, tags.m_err, tags.m_cancelled, tags.m_failed }, [](int tag) { return tag > MAX_UNION_TAG; }))
				{
					return std::unexpected(m_backend.LimitExceeded("Union tag for 'join' result exceeds the supported maximum", line));
				}
				EmitByte(OpCode::JOIN_WORKER, line);
				EmitOperand(tags.m_ok, line);
				EmitOperand(tags.m_err, line);
				EmitOperand(tags.m_cancelled, line);
				EmitOperand(tags.m_failed, line);
				return {};
			}
			case MidoriIROp::ChannelNew:
				EmitByte(OpCode::CHANNEL_CREATE, line);
				return {};
			case MidoriIROp::Send:
				EmitByte(OpCode::CHANNEL_SEND, line);
				return {};
			case MidoriIROp::Receive:
				EmitByte(OpCode::CHANNEL_RECEIVE, line);
				return {};
			case MidoriIROp::ChannelClose:
				EmitByte(OpCode::CHANNEL_CLOSE, line);
				return {};
			case MidoriIROp::WorkerIsDone:
				EmitByte(OpCode::WORKER_IS_DONE, line);
				return {};
			case MidoriIROp::WorkerCancel:
				EmitByte(OpCode::WORKER_CANCEL, line);
				return {};
			default:
				throw std::logic_error(std::format("the bytecode backend cannot emit {} yet", GetMidoriIROpInfo(instruction.m_op).m_name));
			}
		}

		// The (argument, parameter) pairs a jump must move: a Unit parameter
		// keeps nothing, and an argument already in its parameter's slot is
		// there.
		std::vector<std::pair<MidoriIRValueId, MidoriIRValueId>> Moves(const MidoriIRSuccessor& successor) const
		{
			const std::vector<MidoriIRValueId>& parameters = TargetOf(successor).m_parameters;
			std::vector<std::pair<MidoriIRValueId, MidoriIRValueId>> moves;
			for (size_t index = 0u; index < parameters.size(); index += 1u)
			{
				const MidoriIRValueId argument = successor.m_arguments[index];
				const bool is_in_place = IsInSlot(argument) && m_slots[argument.m_index] == m_slots[parameters[index].m_index];
				if (IsInSlot(parameters[index]) && !is_in_place)
				{
					moves.emplace_back(argument, parameters[index]);
				}
			}
			return moves;
		}

		// Every argument is pushed before any parameter is stored, so a jump
		// that passes one parameter's value to another reads it first. A
		// parameter's slot is written by no other of the block's parameters,
		// which are all live at once, so a pair already in place is left out.
		Emitted EmitMoves(const MidoriIRSuccessor& successor, int line)
		{
			const std::vector<std::pair<MidoriIRValueId, MidoriIRValueId>> moves = Moves(successor);
			const std::vector<MidoriIRValueId> arguments = moves | std::views::keys | std::ranges::to<std::vector>();
			return LoadAll(arguments, line)
				.transform([this, &moves, line]()
				{
					for (const MidoriIRValueId parameter : moves | std::views::values | std::views::reverse)
					{
						Store(parameter, line);
					}
				});
		}

		// A jump to the block laid out next is left out unless code follows it.
		Emitted EmitJumpTo(MidoriIRBlockId from, MidoriIRBlockId target, bool may_fall_through, int line)
		{
			if (may_fall_through && m_next_emitted[from.m_index] == target.m_index)
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

		// JUMP_IF_FALSE leaves the condition, so each path pops it. Gives
		// where the jump's offset goes.
		int EmitConditionJump(int line)
		{
			EmitByte(OpCode::JUMP_IF_FALSE, line);
			const int offset = m_stream.GetByteCodeSize();
			EmitOperand(0, line);
			EmitOperand(0, line);
			EmitByte(OpCode::POP, line);
			return offset;
		}

		// The comparison `condition` was not emitted; this opcode makes it and
		// jumps when it fails.
		int EmitFusedBranch(OpCode op, MidoriIRValueId condition, int line)
		{
			const std::vector<MidoriIRValueId>& operands = m_definitions[condition.m_index]->m_operands;
			EmitByte(op, line);
			if (op == OpCode::IF_LOCAL_LE_INT || op == OpCode::IF_LOCAL_LT_INT)
			{
				EmitOperand(m_slots[operands[0u].m_index], line);
				EmitOperand(static_cast<uint8_t>(ByteConstant(operands[1u]).value()), line);
			}
			else if (op == OpCode::IF_LOCAL_TAG_NOT)
			{
				const bool is_tag_first = TagConstant(operands[1u]).has_value();
				const MidoriIRValueId tag = operands[is_tag_first ? 0u : 1u];
				EmitOperand(m_slots[m_definitions[tag.m_index]->m_operands.front().m_index], line);
				EmitOperand(TagConstant(operands[is_tag_first ? 1u : 0u]).value(), line);
			}
			else if (op == OpCode::IF_LOCAL_GE_LOCAL || op == OpCode::IF_LOCAL_LT_LOCAL || op == OpCode::IF_LOCAL_EQ_LOCAL)
			{
				EmitOperand(m_slots[operands[0u].m_index], line);
				EmitOperand(m_slots[operands[1u].m_index], line);
			}
			const int offset = m_stream.GetByteCodeSize();
			EmitOperand(0, line);
			EmitOperand(0, line);
			return offset;
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
				const std::optional<OpCode> fused = m_fused_branches[instruction.m_operands.front().m_index];
				// A fused comparison leaves nothing to pop, so when the false
				// edge moves nothing it jumps straight to its block, and the
				// true one may fall through.
				const bool jumps_to_false_block = fused.has_value() && Moves(if_false).empty() && if_false.m_block.m_index > block.m_index;
				if (jumps_to_false_block)
				{
					const int when_false = EmitFusedBranch(fused.value(), instruction.m_operands.front(), line);
					m_fixups.push_back(Fixup{ when_false, if_false.m_block, line });
					return EmitMoves(if_true, line)
						.and_then([&]() { return EmitJumpTo(block, if_true.m_block, true, line); });
				}
				return (fused.has_value() ? Emitted() : Load(instruction.m_operands.front(), line))
					.and_then([&]() -> Emitted
					{
						const int when_false = fused.has_value() ? EmitFusedBranch(fused.value(), instruction.m_operands.front(), line) : EmitConditionJump(line);
						return EmitMoves(if_true, line)
							.and_then([&]() { return EmitJumpTo(block, if_true.m_block, false, line); })
							.and_then([&]() { return PatchJump(when_false, m_stream.GetByteCodeSize(), line); })
							.and_then([&]() -> Emitted
							{
								if (!fused.has_value())
								{
									EmitByte(OpCode::POP, line);
								}
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
			case MidoriIROp::Unreachable:
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
								EmitProcedure(OpCode::MAKE_FUNCTION_WIDE, procedure, line);
							});
					}
					if (std::holds_alternative<MidoriIRGlobalSlot>(instruction.m_immediate))
					{
						EmitGlobal(OpCode::GET_GLOBAL_WIDE, m_backend.GlobalOperand(std::get<MidoriIRGlobalSlot>(instruction.m_immediate)), line);
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

MidoriResult::BytecodeBackendResult BytecodeBackend::Emit() &&
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
		// A specialization of another module's generic runs that module's
		// source, and its runtime errors point there.
		const std::string& owner = ir_function.m_source_module.empty() ? module.m_name : ir_function.m_source_module;
		const size_t index = m_procedure_indices[function];
		bytecode.m_procedures[index] = std::move(procedure).value();
		bytecode.m_procedure_names[index] = std::format("{}@{}", ir_function.m_name, owner);
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
			const size_t procedure_index = exported.m_function.has_value() ? ProcedureIndex(exported.m_function.value()) : 0uz;
			return BytecodeModule::ExportedSymbol(exported.m_name, procedure_index, global_index, exported.m_kind, MakeSourceProvenance(exported.m_token));
		})
		| std::ranges::to<std::vector>();
	for (const auto& [library, symbols] : m_lowered.m_native_imports)
	{
		std::error_code directory_error;
		const std::filesystem::path module_directory = std::filesystem::absolute(std::filesystem::path(m_file_name), directory_error).parent_path();
		bytecode.m_native_libraries.push_back(NativeLibraryImport
		{
			.m_name = library,
			.m_symbols = std::vector<std::string>(symbols.begin(), symbols.end()),
			.m_hint_directories = { directory_error ? std::filesystem::path(m_file_name).parent_path().string() : module_directory.string() }
		});
	}
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

size_t BytecodeBackend::CaptureCount(MidoriIRFunctionId function) const
{
	return m_lowered.m_module.Function(function).m_capture_types.size();
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
