#include "Compiler/MidoriIROptimizer/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRBodyCopy.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// A callee this small is inlined wherever it is called; its size counts
	// what it runs besides constants.
	constexpr size_t s_max_inlined_size = 24u;
	// A caller stops taking bodies past this size.
	constexpr size_t s_max_caller_size = 4000u;

	std::optional<MidoriIRFunctionId> DirectCallee(const MidoriIRInstruction& instruction)
	{
		const bool is_call = instruction.m_op == MidoriIROp::Call || instruction.m_op == MidoriIROp::TailCall;
		if (!is_call || !std::holds_alternative<MidoriIRFunctionId>(instruction.m_immediate))
		{
			return std::nullopt;
		}
		return std::get<MidoriIRFunctionId>(instruction.m_immediate);
	}

	// The functions each calls directly, and how many times the module names
	// each one anywhere.
	struct CallGraph
	{
		std::vector<std::vector<uint32_t>> m_callees;
		std::vector<uint32_t> m_references;

		explicit CallGraph(const MidoriIRModule& module)
			: m_callees(module.m_functions.size()),
			m_references(module.m_functions.size(), 0u)
		{
			for (uint32_t caller = 0u; caller < module.m_functions.size(); caller += 1u)
			{
				for (const MidoriIRBlock& block : module.m_functions[caller].m_blocks)
				{
					for (const MidoriIRInstruction& instruction : block.m_instructions)
					{
						if (!std::holds_alternative<MidoriIRFunctionId>(instruction.m_immediate))
						{
							continue;
						}
						const uint32_t named = std::get<MidoriIRFunctionId>(instruction.m_immediate).m_index;
						m_references[named] += 1u;
						if (DirectCallee(instruction).has_value())
						{
							m_callees[caller].push_back(named);
						}
					}
				}
			}
			if (module.m_top_level.has_value())
			{
				m_references[module.m_top_level->m_index] += 1u;
			}
		}

		// Tarjan's strongly connected components, callees' components before
		// their callers'. Gives each function's component.
		std::pair<std::vector<uint32_t>, std::vector<uint32_t>> Components() const
		{
			const uint32_t count = static_cast<uint32_t>(m_callees.size());
			std::vector<uint32_t> order;
			std::vector<uint32_t> component(count, UINT32_MAX);
			std::vector<uint32_t> index(count, UINT32_MAX);
			std::vector<uint32_t> low(count, 0u);
			std::vector<bool> on_stack(count, false);
			std::vector<uint32_t> stack;
			uint32_t next_index = 0u;
			uint32_t next_component = 0u;
			for (uint32_t root = 0u; root < count; root += 1u)
			{
				if (index[root] != UINT32_MAX)
				{
					continue;
				}
				std::vector<std::pair<uint32_t, size_t>> frames = { { root, 0u } };
				index[root] = low[root] = next_index++;
				stack.push_back(root);
				on_stack[root] = true;
				while (!frames.empty())
				{
					auto& [function, next] = frames.back();
					if (next < m_callees[function].size())
					{
						const uint32_t callee = m_callees[function][next];
						next += 1u;
						if (index[callee] == UINT32_MAX)
						{
							index[callee] = low[callee] = next_index++;
							stack.push_back(callee);
							on_stack[callee] = true;
							frames.emplace_back(callee, 0u);
						}
						else if (on_stack[callee])
						{
							low[function] = std::min(low[function], index[callee]);
						}
						continue;
					}
					const uint32_t finished = function;
					frames.pop_back();
					if (!frames.empty())
					{
						low[frames.back().first] = std::min(low[frames.back().first], low[finished]);
					}
					if (low[finished] != index[finished])
					{
						continue;
					}
					while (true)
					{
						const uint32_t member = stack.back();
						stack.pop_back();
						on_stack[member] = false;
						component[member] = next_component;
						order.push_back(member);
						if (member == finished)
						{
							break;
						}
					}
					next_component += 1u;
				}
			}
			return { std::move(order), std::move(component) };
		}
	};

	// A call's result is the parameter of a block that takes what follows
	// it; each return jumps there, and each tail call makes its call there
	// first. A tail call's callee's returns and tail calls are its caller's.
	void InlineAt(MidoriIRModule& module, uint32_t caller_index, MidoriIRSite site)
	{
		MidoriIRFunction& caller = module.m_functions[caller_index];
		const MidoriIRInstruction call = caller.m_blocks[site.m_block].m_instructions[site.m_position];
		const MidoriIRFunction& callee = module.Function(std::get<MidoriIRFunctionId>(call.m_immediate));
		// Another module's source has other lines: the call's own stands for
		// all of them, as the call did.
		const std::optional<int> line = callee.m_source_module == caller.m_source_module ? std::nullopt : std::optional<int>(call.m_line);

		if (call.m_op == MidoriIROp::TailCall)
		{
			const MidoriIRCopiedBody body = MidoriIRBodyCopy::Copy(callee, caller, site.m_block + 1u, line);
			caller.m_blocks[site.m_block].m_instructions.back() = MidoriIRAnalysis::Jump(MidoriIRSuccessor(body.Entry(), call.m_operands), call.m_line);
			return;
		}

		const MidoriIRBlockId continuation = MidoriIRAnalysis::InsertBlock(caller, site.m_block + 1u);
		std::vector<MidoriIRInstruction>& instructions = caller.m_blocks[site.m_block].m_instructions;
		MidoriIRBlock& after = caller.m_blocks[continuation.m_index];
		after.m_parameters = { call.m_result.value() };
		after.m_instructions.assign(std::make_move_iterator(instructions.begin() + site.m_position + 1), std::make_move_iterator(instructions.end()));
		instructions.erase(instructions.begin() + site.m_position, instructions.end());

		const MidoriIRCopiedBody body = MidoriIRBodyCopy::Copy(callee, caller, site.m_block + 1u, line);
		const MidoriIRBlockId rejoin{ continuation.m_index + body.m_count };
		caller.m_blocks[site.m_block].m_instructions.push_back(MidoriIRAnalysis::Jump(MidoriIRSuccessor(body.Entry(), call.m_operands), call.m_line));
		for (uint32_t block = body.m_first; block < body.m_first + body.m_count; block += 1u)
		{
			MidoriIRInstruction terminator = std::move(caller.m_blocks[block].m_instructions.back());
			caller.m_blocks[block].m_instructions.pop_back();
			std::vector<MidoriIRInstruction>& copied = caller.m_blocks[block].m_instructions;
			switch (terminator.m_op)
			{
			case MidoriIROp::Return:
				copied.push_back(MidoriIRAnalysis::Jump(MidoriIRSuccessor(rejoin, terminator.m_operands), terminator.m_line));
				break;
			case MidoriIROp::TailCall:
			{
				// The callee's frame was gone by the time this call ran, so
				// a trace through it shows the call that reached the callee.
				const std::shared_ptr<MidoriType> type = MidoriIRBodyCopy::CalleeReturnType(module, caller, terminator);
				const MidoriIRValueId result = MidoriIRAnalysis::AddValue(caller, type);
				copied.push_back(MidoriIRBodyCopy::AsCall(terminator, result, type));
				copied.back().m_line = call.m_line;
				copied.push_back(MidoriIRAnalysis::IsNever(type) ? MidoriIRAnalysis::Unreachable(terminator.m_line) : MidoriIRAnalysis::Jump(MidoriIRSuccessor(rejoin, { result }), terminator.m_line));
				break;
			}
			default:
				copied.push_back(std::move(terminator));
				break;
			}
		}
	}

	bool IsInlinable(const MidoriIRModule& module, const CallGraph& graph, const std::vector<uint32_t>& components, uint32_t caller, const MidoriIRInstruction& call)
	{
		const std::optional<MidoriIRFunctionId> callee_id = DirectCallee(call);
		if (!callee_id.has_value() || callee_id->m_index == caller)
		{
			return false;
		}
		const MidoriIRFunction& callee = module.Function(callee_id.value());
		if (!callee.m_capture_types.empty() || MidoriIRAnalysis::IsNever(callee.m_return_type) || !MidoriIRAnalysis::IsFrameTransparent(callee))
		{
			return false;
		}
		// Within a cycle of calls only a tail call is inlined, which leaves
		// no call behind to inline again: it can make a mutual tail call a
		// loop.
		if (components[callee_id->m_index] == components[caller] && call.m_op != MidoriIROp::TailCall)
		{
			return false;
		}
		const bool is_only_use = graph.m_references[callee_id->m_index] == 1u && components[callee_id->m_index] != components[caller];
		return is_only_use || MidoriIRAnalysis::Size(callee) <= s_max_inlined_size;
	}

	void InlineInto(MidoriIRModule& module, const CallGraph& graph, const std::vector<uint32_t>& components, uint32_t caller)
	{
		std::vector<MidoriIRSite> sites;
		const MidoriIRFunction& function = module.m_functions[caller];
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MidoriIRInstruction>& instructions = function.m_blocks[block].m_instructions;
			for (uint32_t position = 0u; position < instructions.size(); position += 1u)
			{
				if (IsInlinable(module, graph, components, caller, instructions[position]))
				{
					sites.push_back(MidoriIRSite{ block, position });
				}
			}
		}
		// From the last site back, so that inlining one moves no site not yet
		// inlined.
		for (const MidoriIRSite& site : sites | std::views::reverse)
		{
			if (MidoriIRAnalysis::Size(module.m_functions[caller]) > s_max_caller_size)
			{
				break;
			}
			InlineAt(module, caller, site);
		}
	}
}

std::string_view InliningPass::Name() const
{
	return "Inlining";
}

void InliningPass::Run(MidoriIRModule& module) const
{
	const CallGraph graph(module);
	const auto [order, components] = graph.Components();
	for (const uint32_t caller : order)
	{
		InlineInto(module, graph, components, caller);
	}
}
