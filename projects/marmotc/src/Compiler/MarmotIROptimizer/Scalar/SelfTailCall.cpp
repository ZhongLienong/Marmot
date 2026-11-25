#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRModuleFacts.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"
#include "Support/Optional.h"

#include <ranges>
#include <utility>

namespace
{
	// The arguments of a tail call of `function` itself, or nothing when it
	// calls something else.
	MarmotCompiler::Optional<std::vector<MarmotIRValueId>> SelfCallArguments(const MarmotIRModuleFacts& facts, MarmotIRFunctionId function, const MarmotIRFunction& body, const std::vector<MarmotCompiler::Optional<MarmotIRSite>>& sites, const MarmotIRInstruction& tail_call)
	{
		if (std::holds_alternative<MarmotIRFunctionId>(tail_call.m_immediate))
		{
			return std::get<MarmotIRFunctionId>(tail_call.m_immediate) == function ? MarmotCompiler::Optional(tail_call.m_operands) : MarmotCompiler::None;
		}
		if (std::holds_alternative<MarmotIRGlobalSlot>(tail_call.m_immediate))
		{
			return facts.GlobalFunction(std::get<MarmotIRGlobalSlot>(tail_call.m_immediate)) == function ? MarmotCompiler::Optional(tail_call.m_operands) : MarmotCompiler::None;
		}
		const MarmotIRInstruction* callee = MarmotIRAnalysis::Definition(body, sites, tail_call.m_operands.front());
		if (callee == nullptr || callee->m_op != MarmotIROp::GetCapture || !facts.IsSelfCapture(function, std::get<MarmotIRIndex>(callee->m_immediate).m_value))
		{
			return MarmotCompiler::None;
		}
		return std::vector<MarmotIRValueId>(tail_call.m_operands.begin() + 1, tail_call.m_operands.end());
	}

	// The entry keeps the function's parameters and jumps to a header that
	// takes them as its own, which the loop jumps back to.
	bool MakeLoop(const MarmotIRModuleFacts& facts, MarmotIRFunctionId function_id, MarmotIRFunction& function)
	{
		const std::vector<MarmotCompiler::Optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
		std::vector<std::pair<MarmotIRSite, std::vector<MarmotIRValueId>>> self_calls;
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const MarmotIRInstruction& terminator = function.m_blocks[block].m_instructions.back();
			if (terminator.m_op != MarmotIROp::TailCall)
			{
				continue;
			}
			MarmotCompiler::Optional<std::vector<MarmotIRValueId>> arguments = SelfCallArguments(facts, function_id, function, sites, terminator);
			if (arguments.HasValue())
			{
				self_calls.emplace_back(MarmotIRSite{ block, static_cast<uint32_t>(function.m_blocks[block].m_instructions.size() - 1u) }, std::move(arguments).Value());
			}
		}
		if (self_calls.empty())
		{
			return false;
		}

		const MarmotIRBlockId header = MarmotIRAnalysis::InsertBlock(function, 1u);
		MarmotIRBlock& entry = function.Block(MarmotIRFunction::s_entry_block);
		std::vector<MarmotCompiler::Optional<MarmotIRValueId>> replacements(function.m_values.size());
		std::vector<MarmotIRValueId> header_parameters;
		for (const MarmotIRValueId parameter : std::vector<MarmotIRValueId>(entry.m_parameters))
		{
			const MarmotIRValue value = function.Value(parameter);
			const MarmotIRValueId header_parameter = MarmotIRAnalysis::AddValue(function, value.m_type, value.m_name);
			header_parameters.push_back(header_parameter);
			replacements[parameter.m_index] = header_parameter;
		}
		MarmotIRBlock& moved_entry = function.Block(MarmotIRFunction::s_entry_block);
		MarmotIRBlock& loop = function.Block(header);
		loop.m_parameters = header_parameters;
		loop.m_instructions = std::move(moved_entry.m_instructions);
		const int line = loop.m_instructions.front().m_line;
		for (auto& [site, arguments] : self_calls)
		{
			// The entry's instructions moved to the header, and every block
			// after the entry moved up by one.
			const uint32_t block = site.m_block == MarmotIRFunction::s_entry_block.m_index ? header.m_index : site.m_block + 1u;
			MarmotIRInstruction& terminator = function.m_blocks[block].m_instructions.back();
			terminator = MarmotIRAnalysis::Jump(MarmotIRSuccessor(header, std::move(arguments)), terminator.m_line);
		}
		MarmotIRAnalysis::ReplaceUses(function, replacements);
		moved_entry.m_instructions.clear();
		moved_entry.m_instructions.push_back(MarmotIRAnalysis::Jump(MarmotIRSuccessor(header, moved_entry.m_parameters), line));
		return true;
	}
}

bool RunSelfTailCall(MarmotIRModule& module)
{
	const MarmotIRModuleFacts facts(module);
	bool changed = false;
	for (uint32_t function = 0u; function < module.m_functions.size(); function += 1u)
	{
		changed = MakeLoop(facts, MarmotIRFunctionId{ function }, module.m_functions[function]) || changed;
	}
	return changed;
}
