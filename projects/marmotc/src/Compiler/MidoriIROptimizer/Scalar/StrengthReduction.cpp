#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <bit>
#include <utility>

namespace
{
	// What an instruction reduces to: one of its operands, a constant, or
	// another operation on one operand, with a constant second when needed.
	struct Reduction
	{
		enum class Kind : uint8_t
		{
			Operand,
			Constant,
			Unary,
			WithConstant
		};

		Kind m_kind;
		MidoriIRValueId m_operand{ 0u };
		MidoriIROp m_op = MidoriIROp::Const;
		MidoriIRImmediate m_constant;

		static Reduction ToOperand(MidoriIRValueId operand)
		{
			return Reduction{ Kind::Operand, operand, MidoriIROp::Const, {} };
		}

		static Reduction ToConstant(MidoriIRImmediate constant)
		{
			return Reduction{ Kind::Constant, MidoriIRValueId{ 0u }, MidoriIROp::Const, std::move(constant) };
		}

		static Reduction ToUnary(MidoriIROp op, MidoriIRValueId operand)
		{
			return Reduction{ Kind::Unary, operand, op, {} };
		}

		static Reduction ToBinary(MidoriIROp op, MidoriIRValueId operand, MidoriIRImmediate constant)
		{
			return Reduction{ Kind::WithConstant, operand, op, std::move(constant) };
		}
	};

	class Reducer
	{
	private:
		const MidoriIRFunction& m_function;
		std::vector<std::optional<MidoriIRSite>> m_sites;

	public:
		explicit Reducer(const MidoriIRFunction& function)
			: m_function(function),
			m_sites(MidoriIRAnalysis::DefinitionSites(function))
		{
		}

		std::optional<Reduction> Reduce(const MidoriIRInstruction& instruction) const
		{
			using enum MidoriIROp;
			const std::vector<MidoriIRValueId>& operands = instruction.m_operands;
			switch (instruction.m_op)
			{
			case AddInt:
			case AddByte:
			case AddWord:
			case BitOrInt:
			case BitOrByte:
			case BitOrWord:
			case BitXorInt:
			case BitXorByte:
			case BitXorWord:
				return IsInteger(operands[1u], 0) ? Reduction::ToOperand(operands[0u])
					: IsInteger(operands[0u], 0) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case SubInt:
			case SubByte:
			case SubWord:
				if (IsInteger(operands[1u], 0))
				{
					return Reduction::ToOperand(operands[0u]);
				}
				if (instruction.m_op == SubInt && IsInteger(operands[0u], 0))
				{
					return Reduction::ToUnary(NegInt, operands[1u]);
				}
				return operands[0u] == operands[1u] ? std::optional(Reduction::ToConstant(Zero(operands[0u]))) : std::nullopt;
			case MulInt:
				return ReduceMultiply(operands[0u], operands[1u]).or_else([&]() { return ReduceMultiply(operands[1u], operands[0u]); });
			case MulByte:
			case MulWord:
				return IsInteger(operands[0u], 0) || IsInteger(operands[1u], 0) ? Reduction::ToConstant(Zero(operands[0u]))
					: IsInteger(operands[0u], 1) ? std::optional(Reduction::ToOperand(operands[1u]))
					: IsInteger(operands[1u], 1) ? std::optional(Reduction::ToOperand(operands[0u]))
					: std::nullopt;
			case DivInt:
				return IsInteger(operands[1u], 1) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsInteger(operands[1u], -1) ? std::optional(Reduction::ToUnary(NegInt, operands[0u]))
					: std::nullopt;
			case DivByte:
			case DivWord:
				return IsInteger(operands[1u], 1) ? std::optional(Reduction::ToOperand(operands[0u])) : std::nullopt;
			case ModInt:
				return IsInteger(operands[1u], 1) || IsInteger(operands[1u], -1) ? std::optional(Reduction::ToConstant(int64_t{ 0 })) : std::nullopt;
			case ModByte:
			case ModWord:
				return IsInteger(operands[1u], 1) ? std::optional(Reduction::ToConstant(Zero(operands[0u]))) : std::nullopt;
			case BitAndInt:
			case BitAndByte:
			case BitAndWord:
				return IsInteger(operands[1u], 0) || IsInteger(operands[0u], 0) ? Reduction::ToConstant(Zero(operands[0u]))
					: IsInteger(operands[1u], -1) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsInteger(operands[0u], -1) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case ShlInt:
			case ShlByte:
			case ShlWord:
			case ShrInt:
			case ShrByte:
			case ShrWord:
				return IsInteger(operands[1u], 0) ? std::optional(Reduction::ToOperand(operands[0u])) : std::nullopt;
			case EqInt:
			case EqByte:
			case EqWord:
			case LeInt:
			case LeByte:
			case LeWord:
			case GeInt:
			case GeByte:
			case GeWord:
				return operands[0u] == operands[1u] ? std::optional(Reduction::ToConstant(true)) : std::nullopt;
			case NeInt:
			case NeByte:
			case NeWord:
			case LtInt:
			case LtByte:
			case LtWord:
			case GtInt:
			case GtByte:
			case GtWord:
				return operands[0u] == operands[1u] ? std::optional(Reduction::ToConstant(false)) : std::nullopt;
			// Only what holds for NaN and -0.0 too: x * 1.0, x / 1.0, x - 0.0
			// and x + -0.0 are x, but x + 0.0 is 0.0 for x = -0.0.
			case MulFloat:
				return IsFloat(operands[1u], 1.0) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsFloat(operands[0u], 1.0) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case DivFloat:
				return IsFloat(operands[1u], 1.0) ? std::optional(Reduction::ToOperand(operands[0u])) : std::nullopt;
			case SubFloat:
				return IsFloat(operands[1u], 0.0) ? std::optional(Reduction::ToOperand(operands[0u])) : std::nullopt;
			case AddFloat:
				return IsFloat(operands[1u], -0.0) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsFloat(operands[0u], -0.0) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case EqBool:
				return IsBool(operands[1u], true) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsBool(operands[0u], true) ? std::optional(Reduction::ToOperand(operands[1u]))
					: IsBool(operands[1u], false) ? std::optional(Reduction::ToUnary(NotBool, operands[0u]))
					: IsBool(operands[0u], false) ? std::optional(Reduction::ToUnary(NotBool, operands[1u]))
					: std::nullopt;
			case NeBool:
				return IsBool(operands[1u], false) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsBool(operands[0u], false) ? std::optional(Reduction::ToOperand(operands[1u]))
					: IsBool(operands[1u], true) ? std::optional(Reduction::ToUnary(NotBool, operands[0u]))
					: IsBool(operands[0u], true) ? std::optional(Reduction::ToUnary(NotBool, operands[1u]))
					: std::nullopt;
			case NotBool:
			{
				const MidoriIRInstruction* inner = Definition(operands[0u]);
				return inner != nullptr && inner->m_op == NotBool ? std::optional(Reduction::ToOperand(inner->m_operands[0u])) : std::nullopt;
			}
			default:
				return std::nullopt;
			}
		}

	private:
		const MidoriIRInstruction* Definition(MidoriIRValueId value) const
		{
			return MidoriIRAnalysis::Definition(m_function, m_sites, value);
		}

		const MidoriIRImmediate* Constant(MidoriIRValueId value) const
		{
			const MidoriIRInstruction* definition = Definition(value);
			return definition != nullptr && definition->m_op == MidoriIROp::Const ? &definition->m_immediate : nullptr;
		}

		bool IsInteger(MidoriIRValueId value, int64_t expected) const
		{
			const MidoriIRImmediate* constant = Constant(value);
			if (constant == nullptr)
			{
				return false;
			}
			return std::visit([expected](const auto& immediate)
			{
				using Immediate = std::decay_t<decltype(immediate)>;
				if constexpr (std::is_same_v<Immediate, int64_t>)
				{
					return immediate == expected;
				}
				else if constexpr (std::is_same_v<Immediate, MidoriIRByte>)
				{
					return immediate.m_value == static_cast<uint8_t>(expected);
				}
				else if constexpr (std::is_same_v<Immediate, MidoriIRWord>)
				{
					return immediate.m_value == static_cast<uint64_t>(expected);
				}
				else
				{
					return false;
				}
			}, *constant);
		}

		MidoriIRImmediate Zero(MidoriIRValueId value) const
		{
			const std::shared_ptr<MidoriType>& type = m_function.TypeOf(value);
			if (MidoriIRSameType(type, MidoriIRScalarType(MidoriIRScalar::Byte)))
			{
				return MidoriIRByte{ 0u };
			}
			if (MidoriIRSameType(type, MidoriIRScalarType(MidoriIRScalar::Word)))
			{
				return MidoriIRWord{ 0u };
			}
			return int64_t{ 0 };
		}

		// By bits, so that 0.0 and -0.0 are told apart.
		bool IsFloat(MidoriIRValueId value, double expected) const
		{
			const MidoriIRImmediate* constant = Constant(value);
			return constant != nullptr && std::holds_alternative<double>(*constant) && std::bit_cast<uint64_t>(std::get<double>(*constant)) == std::bit_cast<uint64_t>(expected);
		}

		bool IsBool(MidoriIRValueId value, bool expected) const
		{
			const MidoriIRImmediate* constant = Constant(value);
			return constant != nullptr && std::holds_alternative<bool>(*constant) && std::get<bool>(*constant) == expected;
		}

		// x * 0, x * 1, x * -1 and x * 2^n, with the constant second.
		std::optional<Reduction> ReduceMultiply(MidoriIRValueId value, MidoriIRValueId factor) const
		{
			const MidoriIRImmediate* constant = Constant(factor);
			if (constant == nullptr || !std::holds_alternative<int64_t>(*constant))
			{
				return std::nullopt;
			}
			const int64_t multiplier = std::get<int64_t>(*constant);
			if (multiplier == 0)
			{
				return Reduction::ToConstant(int64_t{ 0 });
			}
			if (multiplier == 1)
			{
				return Reduction::ToOperand(value);
			}
			if (multiplier == -1)
			{
				return Reduction::ToUnary(MidoriIROp::NegInt, value);
			}
			if (multiplier > 0 && std::has_single_bit(static_cast<uint64_t>(multiplier)))
			{
				return Reduction::ToBinary(MidoriIROp::ShlInt, value, static_cast<int64_t>(std::countr_zero(static_cast<uint64_t>(multiplier))));
			}
			return std::nullopt;
		}
	};

	std::shared_ptr<MidoriType> ConstantType(const MidoriIRImmediate& constant)
	{
		if (std::holds_alternative<MidoriIRByte>(constant))
		{
			return MidoriIRScalarType(MidoriIRScalar::Byte);
		}
		if (std::holds_alternative<MidoriIRWord>(constant))
		{
			return MidoriIRScalarType(MidoriIRScalar::Word);
		}
		return std::holds_alternative<bool>(constant) ? MidoriIRScalarType(MidoriIRScalar::Bool) : MidoriIRScalarType(MidoriIRScalar::Int);
	}

	bool ReduceFunction(MidoriIRFunction& function)
	{
		bool changed = false;
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const Reducer reducer(function);
			std::vector<MidoriIRInstruction> reduced;
			for (const MidoriIRInstruction& instruction : function.m_blocks[block].m_instructions)
			{
				const std::optional<Reduction> reduction = instruction.m_result.has_value() ? reducer.Reduce(instruction) : std::nullopt;
				if (!reduction.has_value())
				{
					reduced.push_back(instruction);
					continue;
				}
				const MidoriIRValueId result = instruction.m_result.value();
				changed = true;
				switch (reduction->m_kind)
				{
				case Reduction::Kind::Operand:
					// The instruction stays until nothing uses it; a fault it
					// could raise was already ruled out by the constant.
					replacements[result.m_index] = reduction->m_operand;
					reduced.push_back(instruction);
					break;
				case Reduction::Kind::Constant:
					reduced.emplace_back(MidoriIROp::Const, result, ConstantType(reduction->m_constant), std::vector<MidoriIRValueId>{}, reduction->m_constant, std::vector<MidoriIRSuccessor>{}, instruction.m_line);
					break;
				case Reduction::Kind::Unary:
					reduced.emplace_back(reduction->m_op, result, instruction.m_type, std::vector<MidoriIRValueId>{ reduction->m_operand }, MidoriIRImmediate{}, std::vector<MidoriIRSuccessor>{}, instruction.m_line);
					break;
				case Reduction::Kind::WithConstant:
				{
					const std::shared_ptr<MidoriType> type = ConstantType(reduction->m_constant);
					const MidoriIRValueId constant = MidoriIRAnalysis::AddValue(function, type);
					reduced.emplace_back(MidoriIROp::Const, constant, type, std::vector<MidoriIRValueId>{}, reduction->m_constant, std::vector<MidoriIRSuccessor>{}, instruction.m_line);
					reduced.emplace_back(reduction->m_op, result, instruction.m_type, std::vector<MidoriIRValueId>{ reduction->m_operand, constant }, MidoriIRImmediate{}, std::vector<MidoriIRSuccessor>{}, instruction.m_line);
					break;
				}
				}
			}
			function.m_blocks[block].m_instructions = std::move(reduced);
		}
		replacements.resize(function.m_values.size());
		MidoriIRAnalysis::ReplaceUses(function, replacements);
		return changed;
	}
}

std::string_view StrengthReductionPass::Name() const
{
	return "StrengthReduction";
}

bool StrengthReductionPass::Run(MidoriIRModule& module) const
{
	return MidoriIRAnalysis::TransformFunctions(module, ReduceFunction);
}
