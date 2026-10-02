#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <map>
#include <ranges>
#include <utility>

namespace
{
	struct Edge
	{
		uint32_t m_block;
		size_t m_successor;
	};

	// The reads of each part of a block parameter, and whether anything else
	// reads it: passing it back to its own block unchanged does not.
	struct Reads
	{
		std::map<uint32_t, std::vector<MidoriIRSite>> m_parts;
		bool m_escapes = false;
	};

	MidoriIROp ReadOf(MidoriIROp constructor)
	{
		return constructor == MidoriIROp::Construct ? MidoriIROp::GetMember : MidoriIROp::TupleGet;
	}

	class Unboxing
	{
	private:
		MidoriIRFunction& m_function;
		std::vector<std::optional<MidoriIRSite>> m_sites;
		std::vector<std::vector<Edge>> m_incoming;

	public:
		explicit Unboxing(MidoriIRFunction& function)
			: m_function(function),
			m_sites(MidoriIRAnalysis::DefinitionSites(function)),
			m_incoming(function.m_blocks.size())
		{
			for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
			{
				const std::vector<MidoriIRSuccessor>& successors = function.m_blocks[block].m_instructions.back().m_successors;
				for (size_t successor = 0u; successor < successors.size(); successor += 1u)
				{
					m_incoming[successors[successor].m_block.m_index].push_back(Edge{ block, successor });
				}
			}
		}

		bool UnboxOne()
		{
			for (uint32_t block = 1u; block < m_function.m_blocks.size(); block += 1u)
			{
				for (size_t index = 0u; index < m_function.m_blocks[block].m_parameters.size(); index += 1u)
				{
					const std::optional<MidoriIROp> constructor = OnlyConstructor(block, index);
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
		MidoriIRSuccessor& SuccessorOf(const Edge& edge)
		{
			return m_function.m_blocks[edge.m_block].m_instructions.back().m_successors[edge.m_successor];
		}

		// Construct or MakeTuple when every edge passes a value made by it, or
		// passes the parameter back to its own block, and at least one makes
		// it.
		std::optional<MidoriIROp> OnlyConstructor(uint32_t block, size_t index)
		{
			const MidoriIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			std::optional<MidoriIROp> constructor;
			for (const Edge& edge : m_incoming[block])
			{
				const MidoriIRValueId argument = SuccessorOf(edge).m_arguments[index];
				if (argument == parameter)
				{
					continue;
				}
				const MidoriIRInstruction* definition = MidoriIRAnalysis::Definition(m_function, m_sites, argument);
				if (definition == nullptr || (definition->m_op != MidoriIROp::Construct && definition->m_op != MidoriIROp::MakeTuple) || constructor.value_or(definition->m_op) != definition->m_op)
				{
					return std::nullopt;
				}
				constructor = definition->m_op;
			}
			return constructor;
		}

		Reads ReadsOf(uint32_t block, size_t index, MidoriIROp read)
		{
			const MidoriIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			Reads reads;
			for (uint32_t user = 0u; user < m_function.m_blocks.size(); user += 1u)
			{
				const std::vector<MidoriIRInstruction>& instructions = m_function.m_blocks[user].m_instructions;
				for (uint32_t position = 0u; position < instructions.size(); position += 1u)
				{
					const MidoriIRInstruction& instruction = instructions[position];
					if (instruction.m_op == read && instruction.m_operands.front() == parameter)
					{
						reads.m_parts[std::get<MidoriIRIndex>(instruction.m_immediate).m_value].push_back(MidoriIRSite{ user, position });
						continue;
					}
					reads.m_escapes = reads.m_escapes || std::ranges::contains(instruction.m_operands, parameter);
					for (const MidoriIRSuccessor& successor : instruction.m_successors)
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
			const MidoriIRValueId parameter = m_function.m_blocks[block].m_parameters[index];
			std::vector<std::optional<MidoriIRValueId>> replacements(m_function.m_values.size());
			std::vector<std::pair<uint32_t, MidoriIRValueId>> parts;
			for (const auto& [part, sites] : reads.m_parts)
			{
				const MidoriIRInstruction& first = m_function.m_blocks[sites.front().m_block].m_instructions[sites.front().m_position];
				const MidoriIRValueId part_parameter = MidoriIRAnalysis::AddValue(m_function, first.m_type, m_function.Value(parameter).m_name);
				parts.emplace_back(part, part_parameter);
				for (const MidoriIRSite& site : sites)
				{
					replacements[m_function.m_blocks[site.m_block].m_instructions[site.m_position].m_result->m_index] = part_parameter;
				}
			}

			for (const Edge& edge : m_incoming[block])
			{
				std::vector<MidoriIRValueId>& arguments = SuccessorOf(edge).m_arguments;
				const MidoriIRValueId whole = arguments[index];
				arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
				if (whole == parameter)
				{
					std::ranges::copy(parts | std::views::values, std::back_inserter(arguments));
					continue;
				}
				const MidoriIRInstruction* constructor = MidoriIRAnalysis::Definition(m_function, m_sites, whole);
				std::ranges::copy(parts | std::views::keys | std::views::transform([constructor](uint32_t part) { return constructor->m_operands[part]; }), std::back_inserter(arguments));
			}
			std::vector<MidoriIRValueId>& parameters = m_function.m_blocks[block].m_parameters;
			parameters.erase(parameters.begin() + static_cast<std::ptrdiff_t>(index));
			std::ranges::copy(parts | std::views::values, std::back_inserter(parameters));

			for (MidoriIRBlock& user : m_function.m_blocks)
			{
				std::erase_if(user.m_instructions, [&](const MidoriIRInstruction& instruction)
				{
					return instruction.m_result.has_value() && instruction.m_result->m_index < replacements.size() && replacements[instruction.m_result->m_index].has_value();
				});
			}
			MidoriIRAnalysis::ReplaceUses(m_function, replacements);
		}
	};

	bool UnboxParameters(MidoriIRFunction& function)
	{
		const size_t blocks_before = function.m_blocks.size();
		MidoriIRAnalysis::RemoveUnreachableBlocks(function);
		bool changed = function.m_blocks.size() != blocks_before;
		while (Unboxing(function).UnboxOne())
		{
			changed = true;
		}
		return changed;
	}
}

std::string_view ParameterUnboxingPass::Name() const
{
	return "ParameterUnboxing";
}

bool ParameterUnboxingPass::Run(MidoriIRModule& module) const
{
	return MidoriIRAnalysis::TransformFunctions(module, UnboxParameters);
}
