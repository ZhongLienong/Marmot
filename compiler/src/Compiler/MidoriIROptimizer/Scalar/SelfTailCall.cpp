#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRModuleFacts.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// The arguments of a tail call of `function` itself, or nothing when it
	// calls something else.
	std::optional<std::vector<MidoriIRValueId>> SelfCallArguments(const MidoriIRModuleFacts& facts, MidoriIRFunctionId function, const MidoriIRFunction& body, const std::vector<std::optional<MidoriIRSite>>& sites, const MidoriIRInstruction& tail_call)
	{
		if (std::holds_alternative<MidoriIRFunctionId>(tail_call.m_immediate))
		{
			return std::get<MidoriIRFunctionId>(tail_call.m_immediate) == function ? std::optional(tail_call.m_operands) : std::nullopt;
		}
		if (std::holds_alternative<MidoriIRGlobalSlot>(tail_call.m_immediate))
		{
			return facts.GlobalFunction(std::get<MidoriIRGlobalSlot>(tail_call.m_immediate)) == function ? std::optional(tail_call.m_operands) : std::nullopt;
		}
		const MidoriIRInstruction* callee = MidoriIRAnalysis::Definition(body, sites, tail_call.m_operands.front());
		if (callee == nullptr || callee->m_op != MidoriIROp::GetCapture || !facts.IsSelfCapture(function, std::get<MidoriIRIndex>(callee->m_immediate).m_value))
		{
			return std::nullopt;
		}
		return std::vector<MidoriIRValueId>(tail_call.m_operands.begin() + 1, tail_call.m_operands.end());
	}

	// The entry keeps the function's parameters and jumps to a header that
	// takes them as its own, which the loop jumps back to.
	void MakeLoop(const MidoriIRModuleFacts& facts, MidoriIRFunctionId function_id, MidoriIRFunction& function)
	{
		const std::vector<std::optional<MidoriIRSite>> sites = MidoriIRAnalysis::DefinitionSites(function);
		std::vector<std::pair<MidoriIRSite, std::vector<MidoriIRValueId>>> self_calls;
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const MidoriIRInstruction& terminator = function.m_blocks[block].m_instructions.back();
			if (terminator.m_op != MidoriIROp::TailCall)
			{
				continue;
			}
			std::optional<std::vector<MidoriIRValueId>> arguments = SelfCallArguments(facts, function_id, function, sites, terminator);
			if (arguments.has_value())
			{
				self_calls.emplace_back(MidoriIRSite{ block, static_cast<uint32_t>(function.m_blocks[block].m_instructions.size() - 1u) }, std::move(arguments).value());
			}
		}
		if (self_calls.empty())
		{
			return;
		}

		const MidoriIRBlockId header = MidoriIRAnalysis::InsertBlock(function, 1u);
		MidoriIRBlock& entry = function.Block(MidoriIRFunction::s_entry_block);
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		std::vector<MidoriIRValueId> header_parameters;
		for (const MidoriIRValueId parameter : std::vector<MidoriIRValueId>(entry.m_parameters))
		{
			const MidoriIRValue value = function.Value(parameter);
			const MidoriIRValueId header_parameter = MidoriIRAnalysis::AddValue(function, value.m_type, value.m_name);
			header_parameters.push_back(header_parameter);
			replacements[parameter.m_index] = header_parameter;
		}
		MidoriIRBlock& moved_entry = function.Block(MidoriIRFunction::s_entry_block);
		MidoriIRBlock& loop = function.Block(header);
		loop.m_parameters = header_parameters;
		loop.m_instructions = std::move(moved_entry.m_instructions);
		const int line = loop.m_instructions.front().m_line;
		for (auto& [site, arguments] : self_calls)
		{
			// The entry's instructions moved to the header, and every block
			// after the entry moved up by one.
			const uint32_t block = site.m_block == MidoriIRFunction::s_entry_block.m_index ? header.m_index : site.m_block + 1u;
			MidoriIRInstruction& terminator = function.m_blocks[block].m_instructions.back();
			terminator = MidoriIRAnalysis::Jump(MidoriIRSuccessor(header, std::move(arguments)), terminator.m_line);
		}
		MidoriIRAnalysis::ReplaceUses(function, replacements);
		moved_entry.m_instructions.clear();
		moved_entry.m_instructions.push_back(MidoriIRAnalysis::Jump(MidoriIRSuccessor(header, moved_entry.m_parameters), line));
	}
}

std::string_view SelfTailCallPass::Name() const
{
	return "SelfTailCall";
}

void SelfTailCallPass::Run(MidoriIRModule& module) const
{
	const MidoriIRModuleFacts facts(module);
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		MakeLoop(facts, MidoriIRFunctionId{ function }, module.m_functions[function]);
	}
}
