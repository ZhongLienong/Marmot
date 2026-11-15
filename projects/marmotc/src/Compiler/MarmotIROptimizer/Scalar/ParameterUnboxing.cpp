#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <map>
#include <ranges>
#include <utility>

namespace
{
	// The reads of each part of a block parameter, and whether anything else
	// reads it: passing it back to its own block unchanged does not.
	struct Reads
	{
		std::map<uint32_t, std::vector<MarmotIRSite>> m_parts;
		bool m_escapes = false;
	};

	MarmotIROp ReadOf(MarmotIROp constructor)
	{
		return constructor == MarmotIROp::Construct ? MarmotIROp::GetMember : MarmotIROp::TupleGet;
	}

	class Unboxing
	{
	private:
		MarmotIRFunction& m_function;
		std::vector<std::optional<MarmotIRSite>> m_sites;
		std::vector<std::vector<MarmotIREdge>> m_incoming;

	public:
		explicit Unboxing(MarmotIRFunction& function)
			: m_function(function),
			m_sites(MarmotIRAnalysis::DefinitionSites(function)),
			m_incoming(MarmotIRAnalysis::IncomingEdges(function))
		{
		}

		bool UnboxOne()
		{
			for (uint32_t block = 1u; block < m_function.m_blocks.size(); block += 1u)
			{
				for (size_t index = 0u; index < m_function.m_blocks[block].m_parameters.size(); index += 1u)
				{
					const std::optional<MarmotIROp> constructor = OnlyConstructor(block, index);
					if (!constructor.has_value())
					{
						continue;
					}
					const Reads reads = ReadsOf(block, index, ReadOf(constructor.value()));
					if (reads.m_escapes)
					{
						continue;
					}
					Unbox(block, index, reads);
					return true;
				}
			}
			return false;
		}

	private:
		// Construct or MakeTuple when every edge passes a value made by it, or
		// passes the parameter back to its own block, and at least one makes
		// it.
		std::optional<MarmotIROp> OnlyConstructor(uint32_t block, size_t index)
		{
			const MarmotIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			std::optional<MarmotIROp> constructor;
			for (const MarmotIREdge& edge : m_incoming[block])
			{
				const MarmotIRValueId argument = MarmotIRAnalysis::SuccessorOf(m_function, edge).m_arguments[index];
				if (argument == parameter)
				{
					continue;
				}
				const MarmotIRInstruction* definition = MarmotIRAnalysis::Definition(m_function, m_sites, argument);
				if (definition == nullptr || (definition->m_op != MarmotIROp::Construct && definition->m_op != MarmotIROp::MakeTuple) || constructor.value_or(definition->m_op) != definition->m_op)
				{
					return std::nullopt;
				}
				constructor = definition->m_op;
			}
			return constructor;
		}

		Reads ReadsOf(uint32_t block, size_t index, MarmotIROp read)
		{
			const MarmotIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			Reads reads;
			for (uint32_t user = 0u; user < m_function.m_blocks.size(); user += 1u)
			{
				const std::vector<MarmotIRInstruction>& instructions = m_function.m_blocks[user].m_instructions;
				for (uint32_t position = 0u; position < instructions.size(); position += 1u)
				{
					const MarmotIRInstruction& instruction = instructions[position];
					if (instruction.m_op == read && instruction.m_operands.front() == parameter)
					{
						reads.m_parts[std::get<MarmotIRIndex>(instruction.m_immediate).m_value].push_back(MarmotIRSite{ user, position });
						continue;
					}
					reads.m_escapes = reads.m_escapes || std::ranges::contains(instruction.m_operands, parameter);
					for (const MarmotIRSuccessor& successor : instruction.m_successors)
					{
						for (size_t argument = 0u; argument < successor.m_arguments.size(); argument += 1u)
						{
							const bool passed_back = successor.m_block.m_index == block && argument == index;
							reads.m_escapes = reads.m_escapes || (successor.m_arguments[argument] == parameter && !passed_back);
						}
					}
				}
			}
			return reads;
		}

		// The parameter becomes one parameter for each part read, and each
		// edge passes what that part was made with instead of the whole.
		void Unbox(uint32_t block, size_t index, const Reads& reads)
		{
			const MarmotIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			std::vector<std::optional<MarmotIRValueId>> replacements(m_function.m_values.size());
			std::vector<std::pair<uint32_t, MarmotIRValueId>> parts;
			for (const auto& [part, sites] : reads.m_parts)
			{
				const MarmotIRInstruction& first = m_function.m_blocks[sites.front().m_block].m_instructions[sites.front().m_position];
				const MarmotIRValueId part_parameter = MarmotIRAnalysis::AddValue(m_function, first.m_type, m_function.Value(parameter).m_name);
				parts.emplace_back(part, part_parameter);
				for (const MarmotIRSite& site : sites)
				{
					replacements[m_function.m_blocks[site.m_block].m_instructions[site.m_position].m_result->m_index] = part_parameter;
				}
			}

			for (const MarmotIREdge& edge : m_incoming[block])
			{
				std::vector<MarmotIRValueId>& arguments = MarmotIRAnalysis::SuccessorOf(m_function, edge).m_arguments;
				const MarmotIRValueId whole = arguments[index];
				arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				if (whole == parameter)
				{
					std::ranges::copy(parts | std::views::values, std::back_inserter(arguments));
					continue;
				}
				const MarmotIRInstruction* constructor = MarmotIRAnalysis::Definition(m_function, m_sites, whole);
				std::ranges::copy(parts | std::views::keys | std::views::transform([constructor](uint32_t part) { return constructor->m_operands[part]; }), std::back_inserter(arguments));
			}
			std::vector<MarmotIRValueId>& parameters = m_function.m_blocks[block].m_parameters;
			parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
			std::ranges::copy(parts | std::views::values, std::back_inserter(parameters));

			for (MarmotIRBlock& user : m_function.m_blocks)
			{
				std::erase_if(user.m_instructions, [&](const MarmotIRInstruction& instruction)
				{
					return instruction.m_result.has_value() && instruction.m_result->m_index < replacements.size() && replacements[instruction.m_result->m_index].has_value();
				});
			}
			MarmotIRAnalysis::ReplaceUses(m_function, replacements);
		}
	};

	bool UnboxParameters(MarmotIRFunction& function)
	{
		const size_t blocks_before = function.m_blocks.size();
		MarmotIRAnalysis::RemoveUnreachableBlocks(function);
		bool changed = function.m_blocks.size() != blocks_before;
		while (Unboxing(function).UnboxOne())
		{
			changed = true;
		}
		return changed;
	}
}

bool RunParameterUnboxing(MarmotIRModule& module)
{
	return MarmotIRAnalysis::TransformFunctions(module, UnboxParameters);
}
