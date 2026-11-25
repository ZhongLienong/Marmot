#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"
#include "Support/Optional.h"

#include <utility>

namespace
{
	// A part read from a value this function made is the value it was made
	// with; once nothing else reads the whole, DCE drops the allocation.
	// Arrays are left alone: appending changes one in place.
	class Forwarding
	{
	private:
		const MarmotIRFunction& m_function;
		std::vector<MarmotCompiler::Optional<MarmotIRSite>> m_sites;

	public:
		explicit Forwarding(const MarmotIRFunction& function)
			: m_function(function),
			m_sites(MarmotIRAnalysis::DefinitionSites(function))
		{
		}

		// The value a read gives, or, for a union's tag, the constant tag.
		MarmotCompiler::Optional<std::variant<MarmotIRValueId, int>> Forward(const MarmotIRInstruction& instruction) const
		{
			if (instruction.m_operands.empty())
			{
				return MarmotCompiler::None;
			}
			const MarmotIRInstruction* whole = Definition(instruction.m_operands.front());
			if (whole == nullptr)
			{
				return MarmotCompiler::None;
			}
			switch (instruction.m_op)
			{
			case MarmotIROp::TupleGet:
				return whole->m_op == MarmotIROp::MakeTuple ? MarmotCompiler::Optional(Part(*whole, Index(instruction))) : MarmotCompiler::None;
			case MarmotIROp::GetMember:
				return Member(*whole, Index(instruction));
			case MarmotIROp::GetTag:
				return whole->m_op == MarmotIROp::MakeUnion ? MarmotCompiler::Optional<std::variant<MarmotIRValueId, int>>(std::get<MarmotIRTag>(whole->m_immediate).m_value) : MarmotCompiler::None;
			case MarmotIROp::UnionField:
			{
				const MarmotIRUnionField field = std::get<MarmotIRUnionField>(instruction.m_immediate);
				const bool is_that_member = whole->m_op == MarmotIROp::MakeUnion && std::get<MarmotIRTag>(whole->m_immediate).m_value == field.m_tag;
				return is_that_member ? MarmotCompiler::Optional(Part(*whole, field.m_index)) : MarmotCompiler::None;
			}
			// MakeRange takes its start, step and end in that order.
			case MarmotIROp::RangeStart:
				return whole->m_op == MarmotIROp::MakeRange ? MarmotCompiler::Optional(Part(*whole, 0u)) : MarmotCompiler::None;
			case MarmotIROp::RangeStep:
				return whole->m_op == MarmotIROp::MakeRange ? MarmotCompiler::Optional(Part(*whole, 1u)) : MarmotCompiler::None;
			case MarmotIROp::RangeEnd:
				return whole->m_op == MarmotIROp::MakeRange ? MarmotCompiler::Optional(Part(*whole, 2u)) : MarmotCompiler::None;
			default:
				return MarmotCompiler::None;
			}
		}

	private:
		const MarmotIRInstruction* Definition(MarmotIRValueId value) const
		{
			return MarmotIRAnalysis::Definition(m_function, m_sites, value);
		}

		static uint32_t Index(const MarmotIRInstruction& instruction)
		{
			return std::get<MarmotIRIndex>(instruction.m_immediate).m_value;
		}

		static std::variant<MarmotIRValueId, int> Part(const MarmotIRInstruction& whole, uint32_t index)
		{
			return whole.m_operands[index];
		}

		// A record update keeps every member but the one it sets, so a member
		// is read through it from the record it updated.
		MarmotCompiler::Optional<std::variant<MarmotIRValueId, int>> Member(const MarmotIRInstruction& whole, uint32_t index) const
		{
			const MarmotIRInstruction* record = &whole;
			while (record->m_op == MarmotIROp::RecordUpdate)
			{
				if (Index(*record) == index)
				{
					return record->m_operands[1u];
				}
				record = Definition(record->m_operands[0u]);
				if (record == nullptr)
				{
					return MarmotCompiler::None;
				}
			}
			return record->m_op == MarmotIROp::Construct ? MarmotCompiler::Optional(Part(*record, index)) : MarmotCompiler::None;
		}
	};

	// Blocks in reverse postorder, so a part read from a part already
	// forwarded, such as a tuple taken out of a union, is read from what made
	// that part.
	bool ReplaceScalars(MarmotIRFunction& function)
	{
		bool changed = false;
		const Forwarding forwarding(function);
		const MarmotIRControlFlow control_flow(function);
		std::vector<MarmotCompiler::Optional<MarmotIRValueId>> replacements(function.m_values.size());
		for (const uint32_t block : control_flow.ReversePostorder())
		{
			for (MarmotIRInstruction& instruction : function.m_blocks[block].m_instructions)
			{
				std::ranges::for_each(instruction.m_operands, [&replacements](MarmotIRValueId& operand) { operand = replacements[operand.m_index].ValueOr(operand); });
				const MarmotCompiler::Optional<std::variant<MarmotIRValueId, int>> forwarded = forwarding.Forward(instruction);
				if (!forwarded.HasValue())
				{
					continue;
				}
				if (std::holds_alternative<MarmotIRValueId>(forwarded.Value()))
				{
					changed = true;
					replacements[instruction.m_result->m_index] = std::get<MarmotIRValueId>(forwarded.Value());
					continue;
				}
				changed = true;
				instruction = MarmotIRAnalysis::Const(instruction.m_result.Value(), MarmotIRScalarType(MarmotIRScalar::Int), static_cast<int64_t>(std::get<int>(forwarded.Value())), instruction.m_line);
			}
		}
		MarmotIRAnalysis::ReplaceUses(function, replacements);
		return changed;
	}
}

bool RunScalarReplacement(MarmotIRModule& module)
{
	return MarmotIRAnalysis::TransformFunctions(module, ReplaceScalars);
}
