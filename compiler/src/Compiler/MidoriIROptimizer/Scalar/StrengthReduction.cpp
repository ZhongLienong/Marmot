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
			return Reduction{ Kind::Operand, operand };
		}

		static Reduction ToConstant(MidoriIRImmediate constant)
		{
			return Reduction{ Kind::Constant, MidoriIRValueId{ 0u }, MidoriIROp::Const, std::move(constant) };
		}

		static Reduction ToUnary(MidoriIROp op, MidoriIRValueId operand)
		{
			return Reduction{ Kind::Unary, operand, op };
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
			case BitOrInt:
			case BitXorInt:
				return IsInt(operands[1u], 0) ? Reduction::ToOperand(operands[0u])
					: IsInt(operands[0u], 0) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case SubInt:
				if (IsInt(operands[1u], 0))
				{
					return Reduction::ToOperand(operands[0u]);
				}
				if (IsInt(operands[0u], 0))
				{
					return Reduction::ToUnary(NegInt, operands[1u]);
				}
				return operands[0u] == operands[1u] ? std::optional(Reduction::ToConstant(int64_t{ 0 })) : std::nullopt;
			case MulInt:
				return ReduceMultiply(operands[0u], operands[1u]).or_else([&]() { return ReduceMultiply(operands[1u], operands[0u]); });
			case DivInt:
				return IsInt(operands[1u], 1) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsInt(operands[1u], -1) ? std::optional(Reduction::ToUnary(NegInt, operands[0u]))
					: std::nullopt;
			case ModInt:
				return IsInt(operands[1u], 1) || IsInt(operands[1u], -1) ? std::optional(Reduction::ToConstant(int64_t{ 0 })) : std::nullopt;
			case BitAndInt:
				return IsInt(operands[1u], 0) || IsInt(operands[0u], 0) ? Reduction::ToConstant(int64_t{ 0 })
					: IsInt(operands[1u], -1) ? std::optional(Reduction::ToOperand(operands[0u]))
					: IsInt(operands[0u], -1) ? std::optional(Reduction::ToOperand(operands[1u]))
					: std::nullopt;
			case ShlInt:
			case ShrInt:
				return IsInt(operands[1u], 0) ? std::optional(Reduction::ToOperand(operands[0u])) : std::nullopt;
			case EqInt:
			case LeInt:
			case GeInt:
				return operands[0u] == operands[1u] ? std::optional(Reduction::ToConstant(true)) : std::nullopt;
			case NeInt:
			case LtInt:
			case GtInt:
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

		bool IsInt(MidoriIRValueId value, int64_t expected) const
		{
			const MidoriIRImmediate* constant = Constant(value);
			return constant != nullptr && std::holds_alternative<int64_t>(*constant) && std::get<int64_t>(*constant) == expected;
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
		return std::holds_alternative<bool>(constant) ? MidoriIRScalarType(MidoriIRScalar::Bool) : MidoriIRScalarType(MidoriIRScalar::Int);
	}

	void ReduceFunction(MidoriIRFunction& function)
	{
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
	}
}

std::string_view StrengthReductionPass::Name() const
{
	return "StrengthReduction";
}

void StrengthReductionPass::Run(MidoriIRModule& module) const
{
	std::ranges::for_each(module.m_functions, ReduceFunction);
}
