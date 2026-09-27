#include "Common/Value/IntegerArithmetic.h"
#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <bit>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace
{
	enum class Level : uint8_t
	{
		// No path the function can take has made it yet.
		Unknown,
		Constant,
		// It may differ from one run to the next.
		Varying
	};

	struct Cell
	{
		Level m_level = Level::Unknown;
		MidoriIRImmediate m_value;

		static Cell Varying()
		{
			return Cell{ Level::Varying, {} };
		}

		static Cell Constant(MidoriIRImmediate value)
		{
			return Cell{ Level::Constant, std::move(value) };
		}
	};

	// Floats compare by their bits: 0.0 and -0.0 are two constants, and a
	// NaN is itself.
	bool SameConstant(const MidoriIRImmediate& left, const MidoriIRImmediate& right)
	{
		if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
		{
			return std::bit_cast<uint64_t>(std::get<double>(left)) == std::bit_cast<uint64_t>(std::get<double>(right));
		}
		return left == right;
	}

	// Lowers `cell` to what it and `incoming` have in common; true if it moved.
	bool Meet(Cell& cell, const Cell& incoming)
	{
		if (incoming.m_level == Level::Unknown || cell.m_level == Level::Varying)
		{
			return false;
		}
		if (cell.m_level == Level::Unknown)
		{
			cell = incoming;
			return true;
		}
		if (incoming.m_level == Level::Varying || !SameConstant(cell.m_value, incoming.m_value))
		{
			cell = Cell::Varying();
			return true;
		}
		return false;
	}

	using Immediates = std::vector<const MidoriIRImmediate*>;

	template<typename T>
	const T& At(const Immediates& operands, size_t index)
	{
		return std::get<T>(*operands[index]);
	}

	uint8_t ByteAt(const Immediates& operands, size_t index)
	{
		return At<MidoriIRByte>(operands, index).m_value;
	}

	uint64_t WordAt(const Immediates& operands, size_t index)
	{
		return At<MidoriIRWord>(operands, index).m_value;
	}

	MidoriIRImmediate Byte(unsigned value)
	{
		return MidoriIRByte{ static_cast<uint8_t>(value) };
	}

	MidoriIRImmediate Word(uint64_t value)
	{
		return MidoriIRWord{ value };
	}

	// What the VM computes, for the operations whose result is the same on
	// every machine. A division by zero, a Float out of an Int's range and
	// the shifts and bit operations of Byte and Word, whose VM forms read
	// their operands as another type, are left to run.
	std::optional<MidoriIRImmediate> Fold(MidoriIROp op, const Immediates& operands)
	{
		using namespace MidoriIntegerArithmetic;
		using enum MidoriIROp;
		const auto int_at = [&operands](size_t index) { return At<int64_t>(operands, index); };
		const auto float_at = [&operands](size_t index) { return At<double>(operands, index); };
		switch (op)
		{
		case AddInt: return Add(int_at(0u), int_at(1u));
		case SubInt: return Subtract(int_at(0u), int_at(1u));
		case MulInt: return Multiply(int_at(0u), int_at(1u));
		case DivInt: return int_at(1u) == 0 ? std::nullopt : std::optional<MidoriIRImmediate>(Divide(int_at(0u), int_at(1u)));
		case ModInt: return int_at(1u) == 0 ? std::nullopt : std::optional<MidoriIRImmediate>(Remainder(int_at(0u), int_at(1u)));
		case NegInt: return Negate(int_at(0u));
		case BitAndInt: return int_at(0u) & int_at(1u);
		case BitOrInt: return int_at(0u) | int_at(1u);
		case BitXorInt: return int_at(0u) ^ int_at(1u);
		case BitNotInt: return ~int_at(0u);
		case ShlInt: return ShiftLeft(int_at(0u), static_cast<uint64_t>(int_at(1u)));
		case ShrInt: return ShiftRight(int_at(0u), static_cast<uint64_t>(int_at(1u)));
		case AddFloat: return float_at(0u) + float_at(1u);
		case SubFloat: return float_at(0u) - float_at(1u);
		case MulFloat: return float_at(0u) * float_at(1u);
		case DivFloat: return float_at(0u) / float_at(1u);
		case ModFloat: return std::fmod(float_at(0u), float_at(1u));
		case NegFloat: return -float_at(0u);
		case AddByte: return Byte(ByteAt(operands, 0u) + ByteAt(operands, 1u));
		case SubByte: return Byte(static_cast<unsigned>(ByteAt(operands, 0u) - ByteAt(operands, 1u)));
		case MulByte: return Byte(static_cast<unsigned>(ByteAt(operands, 0u) * ByteAt(operands, 1u)));
		case DivByte: return ByteAt(operands, 1u) == 0u ? std::nullopt : std::optional<MidoriIRImmediate>(Byte(ByteAt(operands, 0u) / ByteAt(operands, 1u)));
		case ModByte: return ByteAt(operands, 1u) == 0u ? std::nullopt : std::optional<MidoriIRImmediate>(Byte(ByteAt(operands, 0u) % ByteAt(operands, 1u)));
		case AddWord: return Word(WordAt(operands, 0u) + WordAt(operands, 1u));
		case SubWord: return Word(WordAt(operands, 0u) - WordAt(operands, 1u));
		case MulWord: return Word(WordAt(operands, 0u) * WordAt(operands, 1u));
		case DivWord: return WordAt(operands, 1u) == 0u ? std::nullopt : std::optional<MidoriIRImmediate>(Word(WordAt(operands, 0u) / WordAt(operands, 1u)));
		case ModWord: return WordAt(operands, 1u) == 0u ? std::nullopt : std::optional<MidoriIRImmediate>(Word(WordAt(operands, 0u) % WordAt(operands, 1u)));
		case EqInt: return int_at(0u) == int_at(1u);
		case NeInt: return int_at(0u) != int_at(1u);
		case LtInt: return int_at(0u) < int_at(1u);
		case LeInt: return int_at(0u) <= int_at(1u);
		case GtInt: return int_at(0u) > int_at(1u);
		case GeInt: return int_at(0u) >= int_at(1u);
		case EqFloat: return float_at(0u) == float_at(1u);
		case NeFloat: return float_at(0u) != float_at(1u);
		case LtFloat: return float_at(0u) < float_at(1u);
		case LeFloat: return float_at(0u) <= float_at(1u);
		case GtFloat: return float_at(0u) > float_at(1u);
		case GeFloat: return float_at(0u) >= float_at(1u);
		case EqByte: return ByteAt(operands, 0u) == ByteAt(operands, 1u);
		case NeByte: return ByteAt(operands, 0u) != ByteAt(operands, 1u);
		case LtByte: return ByteAt(operands, 0u) < ByteAt(operands, 1u);
		case LeByte: return ByteAt(operands, 0u) <= ByteAt(operands, 1u);
		case GtByte: return ByteAt(operands, 0u) > ByteAt(operands, 1u);
		case GeByte: return ByteAt(operands, 0u) >= ByteAt(operands, 1u);
		case EqWord: return WordAt(operands, 0u) == WordAt(operands, 1u);
		case NeWord: return WordAt(operands, 0u) != WordAt(operands, 1u);
		case LtWord: return WordAt(operands, 0u) < WordAt(operands, 1u);
		case LeWord: return WordAt(operands, 0u) <= WordAt(operands, 1u);
		case GtWord: return WordAt(operands, 0u) > WordAt(operands, 1u);
		case GeWord: return WordAt(operands, 0u) >= WordAt(operands, 1u);
		case EqBool: return At<bool>(operands, 0u) == At<bool>(operands, 1u);
		case NeBool: return At<bool>(operands, 0u) != At<bool>(operands, 1u);
		case NotBool: return !At<bool>(operands, 0u);
		case EqText: return At<std::string>(operands, 0u) == At<std::string>(operands, 1u);
		case NeText: return At<std::string>(operands, 0u) != At<std::string>(operands, 1u);
		case IntToFloat: return static_cast<double>(int_at(0u));
		case FloatToInt:
		{
			// Past an Int's range the conversion is the machine's to choose.
			const double value = float_at(0u);
			const bool fits = value >= -9223372036854775808.0 && value < 9223372036854775808.0;
			return fits ? std::optional<MidoriIRImmediate>(static_cast<int64_t>(value)) : std::nullopt;
		}
		case ByteToInt: return static_cast<int64_t>(ByteAt(operands, 0u));
		case IntToByte: return Byte(static_cast<unsigned>(int_at(0u) & 0xFF));
		case ByteToWord: return Word(ByteAt(operands, 0u));
		case WordToByte: return Byte(static_cast<unsigned>(WordAt(operands, 0u) & 0xFFu));
		case WordToInt: return static_cast<int64_t>(WordAt(operands, 0u));
		case IntToWord: return Word(static_cast<uint64_t>(int_at(0u)));
		case ByteToFloat: return static_cast<double>(ByteAt(operands, 0u));
		case WordToFloat: return static_cast<double>(WordAt(operands, 0u));
		// Text constants are made fresh each time they are loaded, so the
		// result of a growth is as fresh as the constant replacing it.
		case Concat:
		case Extend:
			if (!std::holds_alternative<std::string>(*operands[0u]))
			{
				return std::nullopt;
			}
			return At<std::string>(operands, 0u) + At<std::string>(operands, 1u);
		default:
			return std::nullopt;
		}
	}

	bool IsFoldable(MidoriIROp op)
	{
		const MidoriIRArity arity = GetMidoriIROpInfo(op).m_arity;
		return arity == MidoriIRArity::Unary || arity == MidoriIRArity::Binary || op == MidoriIROp::Concat || op == MidoriIROp::Extend;
	}

	// The type a constant of this immediate has, which the verifier checks a
	// Const's type against: a newtype's constant is its representation's.
	std::shared_ptr<MidoriType> ConstantType(const MidoriIRImmediate& value, const std::shared_ptr<MidoriType>& fallback)
	{
		return std::visit([&fallback](const auto& constant) -> std::shared_ptr<MidoriType>
		{
			using Immediate = std::decay_t<decltype(constant)>;
			if constexpr (std::is_same_v<Immediate, int64_t>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Int);
			}
			else if constexpr (std::is_same_v<Immediate, double>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Float);
			}
			else if constexpr (std::is_same_v<Immediate, bool>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Bool);
			}
			else if constexpr (std::is_same_v<Immediate, std::string>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Text);
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRByte>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Byte);
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRWord>)
			{
				return MidoriIRScalarType(MidoriIRScalar::Word);
			}
			else if constexpr (std::is_same_v<Immediate, std::monostate>)
			{
				return MidoriType::MakeLiteralType<MidoriType::UnitType>();
			}
			else
			{
				return fallback;
			}
		}, value);
	}

	using GlobalConstants = std::unordered_map<uint32_t, MidoriIRImmediate>;

	class Propagation
	{
	private:
		MidoriIRFunction& m_function;
		const GlobalConstants& m_globals;
		std::vector<Cell> m_cells;
		std::vector<bool> m_executable;

	public:
		Propagation(MidoriIRFunction& function, const GlobalConstants& globals)
			: m_function(function),
			m_globals(globals),
			m_cells(function.m_values.size()),
			m_executable(function.m_blocks.size(), false)
		{
			for (const MidoriIRValueId parameter : function.Block(MidoriIRFunction::s_entry_block).m_parameters)
			{
				m_cells[parameter.m_index] = Cell::Varying();
			}
			m_executable[MidoriIRFunction::s_entry_block.m_index] = true;
			const MidoriIRControlFlow control_flow(function);
			bool changed = true;
			while (changed)
			{
				changed = false;
				for (const uint32_t block : control_flow.ReversePostorder())
				{
					if (m_executable[block])
					{
						changed = Visit(block) || changed;
					}
				}
			}
		}

		// The constants global definitions in this function store.
		void CollectGlobals(GlobalConstants& globals) const
		{
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_executable[block])
				{
					continue;
				}
				for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
				{
					if (instruction.m_op != MidoriIROp::GlobalDefine)
					{
						continue;
					}
					const Cell& stored = m_cells[instruction.m_operands.front().m_index];
					if (stored.m_level == Level::Constant && !std::holds_alternative<std::string>(stored.m_value))
					{
						globals.emplace(std::get<MidoriIRGlobalSlot>(instruction.m_immediate).m_value, stored.m_value);
					}
				}
			}
		}

		void Rewrite()
		{
			std::vector<std::optional<MidoriIRValueId>> replacements(m_function.m_values.size());
			for (uint32_t block = 0u; block < m_function.m_blocks.size(); block += 1u)
			{
				if (!m_executable[block])
				{
					continue;
				}
				std::vector<MidoriIRInstruction> constants;
				for (const MidoriIRValueId parameter : m_function.m_blocks[block].m_parameters)
				{
					const Cell& cell = m_cells[parameter.m_index];
					if (cell.m_level != Level::Constant)
					{
						continue;
					}
					const std::shared_ptr<MidoriType> type = ConstantType(cell.m_value, m_function.TypeOf(parameter));
					const MidoriIRValueId constant = MidoriIRAnalysis::AddValue(m_function, type);
					constants.emplace_back(MidoriIROp::Const, constant, type, std::vector<MidoriIRValueId>{}, cell.m_value, std::vector<MidoriIRSuccessor>{}, FirstLine(block));
					replacements[parameter.m_index] = constant;
				}

				std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[block].m_instructions;
				for (MidoriIRInstruction& instruction : instructions)
				{
					if (instruction.m_result.has_value() && instruction.m_op != MidoriIROp::Const && m_cells[instruction.m_result->m_index].m_level == Level::Constant)
					{
						instruction.m_immediate = m_cells[instruction.m_result->m_index].m_value;
						instruction.m_type = ConstantType(instruction.m_immediate, instruction.m_type);
						instruction.m_op = MidoriIROp::Const;
						instruction.m_operands.clear();
						instruction.m_effect = MidoriIREffect();
					}
				}
				MidoriIRInstruction& terminator = instructions.back();
				if (terminator.m_op == MidoriIROp::Branch && m_cells[terminator.m_operands.front().m_index].m_level == Level::Constant)
				{
					const bool condition = std::get<bool>(m_cells[terminator.m_operands.front().m_index].m_value);
					MidoriIRSuccessor taken = std::move(terminator.m_successors[condition ? 0u : 1u]);
					terminator.m_op = MidoriIROp::Jump;
					terminator.m_operands.clear();
					terminator.m_successors.clear();
					terminator.m_successors.push_back(std::move(taken));
				}
				instructions.insert(instructions.begin(), std::make_move_iterator(constants.begin()), std::make_move_iterator(constants.end()));
			}
			MidoriIRAnalysis::ReplaceUses(m_function, replacements);
		}

	private:
		int FirstLine(uint32_t block) const
		{
			return m_function.m_blocks[block].m_instructions.front().m_line;
		}

		bool Visit(uint32_t block)
		{
			bool changed = false;
			for (const MidoriIRInstruction& instruction : m_function.m_blocks[block].m_instructions)
			{
				if (IsMidoriIRTerminator(instruction.m_op))
				{
					changed = VisitTerminator(instruction) || changed;
				}
				else if (instruction.m_result.has_value())
				{
					changed = Meet(m_cells[instruction.m_result->m_index], Evaluate(instruction)) || changed;
				}
			}
			return changed;
		}

		Cell Evaluate(const MidoriIRInstruction& instruction) const
		{
			if (instruction.m_op == MidoriIROp::Const)
			{
				return Cell::Constant(instruction.m_immediate);
			}
			if (instruction.m_op == MidoriIROp::GlobalGet)
			{
				const GlobalConstants::const_iterator global = m_globals.find(std::get<MidoriIRGlobalSlot>(instruction.m_immediate).m_value);
				return global == m_globals.end() ? Cell::Varying() : Cell::Constant(global->second);
			}
			if (!IsFoldable(instruction.m_op))
			{
				return Cell::Varying();
			}

			Immediates operands;
			bool is_unknown = false;
			for (const MidoriIRValueId operand : instruction.m_operands)
			{
				const Cell& cell = m_cells[operand.m_index];
				if (cell.m_level == Level::Varying)
				{
					return Cell::Varying();
				}
				is_unknown = is_unknown || cell.m_level == Level::Unknown;
				operands.push_back(&cell.m_value);
			}
			if (is_unknown)
			{
				return Cell();
			}
			std::optional<MidoriIRImmediate> folded = Fold(instruction.m_op, operands);
			return folded.has_value() ? Cell::Constant(std::move(folded).value()) : Cell::Varying();
		}

		bool VisitTerminator(const MidoriIRInstruction& terminator)
		{
			if (terminator.m_op != MidoriIROp::Branch)
			{
				bool changed = false;
				for (const MidoriIRSuccessor& successor : terminator.m_successors)
				{
					changed = Follow(successor) || changed;
				}
				return changed;
			}
			const Cell& condition = m_cells[terminator.m_operands.front().m_index];
			switch (condition.m_level)
			{
			case Level::Unknown:
				return false;
			case Level::Constant:
				return Follow(terminator.m_successors[std::get<bool>(condition.m_value) ? 0u : 1u]);
			case Level::Varying:
			{
				const bool when_true = Follow(terminator.m_successors[0u]);
				return Follow(terminator.m_successors[1u]) || when_true;
			}
			}
			std::unreachable();
		}

		bool Follow(const MidoriIRSuccessor& successor)
		{
			const uint32_t target = successor.m_block.m_index;
			bool changed = !m_executable[target];
			m_executable[target] = true;
			const std::vector<MidoriIRValueId>& parameters = m_function.m_blocks[target].m_parameters;
			for (size_t index = 0u; index < parameters.size(); index += 1u)
			{
				changed = Meet(m_cells[parameters[index].m_index], m_cells[successor.m_arguments[index].m_index]) || changed;
			}
			return changed;
		}
	};
}

std::string_view SccpPass::Name() const
{
	return "Sccp";
}

void SccpPass::Run(MidoriIRModule& module) const
{
	// Every read of a global comes after its definition, which only the
	// top-level function makes, so what it stores is what every read gives.
	GlobalConstants globals;
	if (module.m_top_level.has_value())
	{
		Propagation(module.Function(module.m_top_level.value()), globals).CollectGlobals(globals);
	}
	for (MidoriIRFunction& function : module.m_functions)
	{
		Propagation(function, globals).Rewrite();
	}
}
