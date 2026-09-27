#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <utility>

namespace
{
	// A part read from a value this function made is the value it was made
	// with; once nothing else reads the whole, DCE drops the allocation.
	// Arrays are left alone: appending changes one in place.
	class Forwarding
	{
	private:
		const MidoriIRFunction& m_function;
		std::vector<std::optional<MidoriIRSite>> m_sites;

	public:
		explicit Forwarding(const MidoriIRFunction& function)
			: m_function(function),
			m_sites(MidoriIRAnalysis::DefinitionSites(function))
		{
		}

		// The value a read gives, or, for a union's tag, the constant tag.
		std::optional<std::variant<MidoriIRValueId, int>> Forward(const MidoriIRInstruction& instruction) const
		{
			if (instruction.m_operands.empty())
			{
				return std::nullopt;
			}
			const MidoriIRInstruction* whole = Definition(instruction.m_operands.front());
			if (whole == nullptr)
			{
				return std::nullopt;
			}
			switch (instruction.m_op)
			{
			case MidoriIROp::TupleGet:
				return whole->m_op == MidoriIROp::MakeTuple ? std::optional(Part(*whole, Index(instruction))) : std::nullopt;
			case MidoriIROp::GetMember:
				return Member(*whole, Index(instruction));
			case MidoriIROp::GetTag:
				return whole->m_op == MidoriIROp::MakeUnion ? std::optional<std::variant<MidoriIRValueId, int>>(std::get<MidoriIRTag>(whole->m_immediate).m_value) : std::nullopt;
			case MidoriIROp::UnionField:
			{
				const MidoriIRUnionField field = std::get<MidoriIRUnionField>(instruction.m_immediate);
				const bool is_that_member = whole->m_op == MidoriIROp::MakeUnion && std::get<MidoriIRTag>(whole->m_immediate).m_value == field.m_tag;
				return is_that_member ? std::optional(Part(*whole, field.m_index)) : std::nullopt;
			}
			// MakeRange takes its start, step and end in that order.
			case MidoriIROp::RangeStart:
				return whole->m_op == MidoriIROp::MakeRange ? std::optional(Part(*whole, 0u)) : std::nullopt;
			case MidoriIROp::RangeStep:
				return whole->m_op == MidoriIROp::MakeRange ? std::optional(Part(*whole, 1u)) : std::nullopt;
			case MidoriIROp::RangeEnd:
				return whole->m_op == MidoriIROp::MakeRange ? std::optional(Part(*whole, 2u)) : std::nullopt;
			default:
				return std::nullopt;
			}
		}

	private:
		const MidoriIRInstruction* Definition(MidoriIRValueId value) const
		{
			return MidoriIRAnalysis::Definition(m_function, m_sites, value);
		}

		static uint32_t Index(const MidoriIRInstruction& instruction)
		{
			return std::get<MidoriIRIndex>(instruction.m_immediate).m_value;
		}

		static std::variant<MidoriIRValueId, int> Part(const MidoriIRInstruction& whole, uint32_t index)
		{
			return whole.m_operands[index];
		}

		// A record update keeps every member but the one it sets, so a member
		// is read through it from the record it updated.
		std::optional<std::variant<MidoriIRValueId, int>> Member(const MidoriIRInstruction& whole, uint32_t index) const
		{
			const MidoriIRInstruction* record = &whole;
			while (record->m_op == MidoriIROp::RecordUpdate)
			{
				if (Index(*record) == index)
				{
					return record->m_operands[1u];
				}
				record = Definition(record->m_operands[0u]);
				if (record == nullptr)
				{
					return std::nullopt;
				}
			}
			return record->m_op == MidoriIROp::Construct ? std::optional(Part(*record, index)) : std::nullopt;
		}
	};

	void ReplaceScalars(MidoriIRFunction& function)
	{
		const Forwarding forwarding(function);
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		for (MidoriIRBlock& block : function.m_blocks)
		{
			for (MidoriIRInstruction& instruction : block.m_instructions)
			{
				const std::optional<std::variant<MidoriIRValueId, int>> forwarded = forwarding.Forward(instruction);
				if (!forwarded.has_value())
				{
					continue;
				}
				if (std::holds_alternative<MidoriIRValueId>(forwarded.value()))
				{
					replacements[instruction.m_result->m_index] = std::get<MidoriIRValueId>(forwarded.value());
					continue;
				}
				instruction.m_op = MidoriIROp::Const;
				instruction.m_immediate = static_cast<int64_t>(std::get<int>(forwarded.value()));
				instruction.m_operands.clear();
				instruction.m_type = MidoriIRScalarType(MidoriIRScalar::Int);
				instruction.m_effect = MidoriIREffect();
			}
		}
		MidoriIRAnalysis::ReplaceUses(function, replacements);
	}
}

std::string_view ScalarReplacementPass::Name() const
{
	return "ScalarReplacement";
}

void ScalarReplacementPass::Run(MidoriIRModule& module) const
{
	std::ranges::for_each(module.m_functions, ReplaceScalars);
}
