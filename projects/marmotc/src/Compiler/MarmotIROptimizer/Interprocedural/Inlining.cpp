#include "Compiler/Lowering/GenericTypes.h"
#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/Interprocedural/MarmotIRBodyCopy.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <ranges>
#include <utility>

namespace
{
	// A callee this small is inlined wherever it is called; its size counts
	// what it runs besides constants.
	constexpr size_t s_max_inlined_size = 24u;
	// A caller stops taking bodies past this size.
	constexpr size_t s_max_caller_size = 4000u;

	std::optional<MarmotIRFunctionId> DirectCallee(const MarmotIRInstruction& instruction)
	{
		const bool is_call = instruction.m_op == MarmotIROp::Call || instruction.m_op == MarmotIROp::TailCall;
		if (!is_call || !std::holds_alternative<MarmotIRFunctionId>(instruction.m_immediate))
		{
			return std::nullopt;
		}
		return std::get<MarmotIRFunctionId>(instruction.m_immediate);
	}

	// The return type of what a call or tail call calls.
	std::shared_ptr<MarmotType> CalleeReturnType(const MarmotIRModule& module, const MarmotIRFunction& caller, const MarmotIRInstruction& call)
	{
		if (std::holds_alternative<MarmotIRFunctionId>(call.m_immediate))
		{
			return module.Function(std::get<MarmotIRFunctionId>(call.m_immediate)).m_return_type;
		}
		const std::shared_ptr<MarmotType> callee = std::holds_alternative<MarmotIRGlobalSlot>(call.m_immediate)
			? module.m_globals[std::get<MarmotIRGlobalSlot>(call.m_immediate).m_value].m_type
			: caller.TypeOf(call.m_operands.front());
		return GenericTypes::RepresentationOf(callee)->GetType<MarmotType::FunctionType>().m_return_type;
	}

	// Turns a tail call into the call it makes, defining `result`: a
	// TailCall's callee is a function, a global or its first operand.
	MarmotIRInstruction AsCall(const MarmotIRInstruction& tail_call, MarmotIRValueId result, std::shared_ptr<MarmotType> type)
	{
		const MarmotIROp op = std::holds_alternative<MarmotIRFunctionId>(tail_call.m_immediate) ? MarmotIROp::Call
			: std::holds_alternative<MarmotIRGlobalSlot>(tail_call.m_immediate) ? MarmotIROp::CallGlobal
			: MarmotIROp::CallValue;
		return MarmotIRInstruction(op, result, std::move(type), tail_call.m_operands, tail_call.m_immediate, {}, tail_call.m_line);
	}

	// The functions each calls directly, and how many times the module names
	// each one anywhere.
	struct CallGraph
	{
		std::vector<std::vector<uint32_t>> m_callees;
		std::vector<uint32_t> m_references;

		explicit CallGraph(const MarmotIRModule& module)
			: m_callees(module.m_functions.size()),
			m_references(module.m_functions.size(), 0u)
		{
			for (uint32_t caller = 0u; caller < module.m_functions.size(); caller += 1u)
			{
				for (const MarmotIRBlock& block : module.m_functions[caller].m_blocks)
				{
					for (const MarmotIRInstruction& instruction : block.m_instructions)
					{
						if (!std::holds_alternative<MarmotIRFunctionId>(instruction.m_immediate))
						{
							continue;
						}
						const uint32_t named = std::get<MarmotIRFunctionId>(instruction.m_immediate).m_index;
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
	void InlineAt(MarmotIRModule& module, uint32_t caller_index, MarmotIRSite site)
	{
		MarmotIRFunction& caller = module.m_functions[caller_index];
		const MarmotIRInstruction call = caller.m_blocks[site.m_block].m_instructions[site.m_position];
		const MarmotIRFunction& callee = module.Function(std::get<MarmotIRFunctionId>(call.m_immediate));
		// Another module's source has other lines: the call's own stands for
		// all of them, as the call did.
		const std::optional<int> line = callee.m_source_module == caller.m_source_module ? std::nullopt : std::optional<int>(call.m_line);

		if (call.m_op == MarmotIROp::TailCall)
		{
			const MarmotIRCopiedBody body = MarmotIRBodyCopy::Copy(callee, caller, site.m_block + 1u, line);
			caller.m_blocks[site.m_block].m_instructions.back() = MarmotIRAnalysis::Jump(MarmotIRSuccessor(body.Entry(), call.m_operands), call.m_line);
			return;
		}

		const MarmotIRBlockId continuation = MarmotIRAnalysis::InsertBlock(caller, site.m_block + 1u);
		std::vector<MarmotIRInstruction>& instructions = caller.m_blocks[site.m_block].m_instructions;
		MarmotIRBlock& after = caller.m_blocks[continuation.m_index];
		after.m_parameters = { call.m_result.value() };
		after.m_instructions.assign(std::make_move_iterator(instructions.begin() + site.m_position + 1), std::make_move_iterator(instructions.end()));
		instructions.erase(instructions.begin() + site.m_position, instructions.end());

		const MarmotIRCopiedBody body = MarmotIRBodyCopy::Copy(callee, caller, site.m_block + 1u, line);
		const MarmotIRBlockId rejoin{ continuation.m_index + body.m_count };
		caller.m_blocks[site.m_block].m_instructions.push_back(MarmotIRAnalysis::Jump(MarmotIRSuccessor(body.Entry(), call.m_operands), call.m_line));
		for (uint32_t block = body.m_first; block < body.m_first + body.m_count; block += 1u)
		{
			MarmotIRInstruction terminator = std::move(caller.m_blocks[block].m_instructions.back());
			caller.m_blocks[block].m_instructions.pop_back();
			std::vector<MarmotIRInstruction>& copied = caller.m_blocks[block].m_instructions;
			switch (terminator.m_op)
			{
			case MarmotIROp::Return:
				copied.push_back(MarmotIRAnalysis::Jump(MarmotIRSuccessor(rejoin, terminator.m_operands), terminator.m_line));
				break;
			case MarmotIROp::TailCall:
			{
				// The callee's frame was gone by the time this call ran, so
				// a trace through it shows the call that reached the callee.
				const std::shared_ptr<MarmotType> type = CalleeReturnType(module, caller, terminator);
				const MarmotIRValueId result = MarmotIRAnalysis::AddValue(caller, type);
				copied.push_back(AsCall(terminator, result, type));
				copied.back().m_line = call.m_line;
				copied.push_back(type->IsType<MarmotType::NeverType>() ? MarmotIRAnalysis::Unreachable(terminator.m_line) : MarmotIRAnalysis::Jump(MarmotIRSuccessor(rejoin, { result }), terminator.m_line));
				break;
			}
			default:
				copied.push_back(std::move(terminator));
				break;
			}
		}
	}

	bool IsInlinable(const MarmotIRModule& module, const CallGraph& graph, const std::vector<uint32_t>& components, uint32_t caller, const MarmotIRInstruction& call)
	{
		const std::optional<MarmotIRFunctionId> callee_id = DirectCallee(call);
		if (!callee_id.has_value() || callee_id->m_index == caller)
		{
			return false;
		}
		const MarmotIRFunction& callee = module.Function(callee_id.value());
		if (!callee.m_capture_types.empty() || callee.m_return_type->IsType<MarmotType::NeverType>() || !MarmotIRAnalysis::IsFrameTransparent(callee))
		{
			return false;
		}
		// Within a cycle of calls only a tail call is inlined, which leaves
		// no call behind to inline again: it can make a mutual tail call a
		// loop.
		if (components[callee_id->m_index] == components[caller] && call.m_op != MarmotIROp::TailCall)
		{
			return false;
		}
		const bool is_only_use = graph.m_references[callee_id->m_index] == 1u && components[callee_id->m_index] != components[caller];
		return is_only_use || MarmotIRAnalysis::Size(callee) <= s_max_inlined_size;
	}

	bool InlineInto(MarmotIRModule& module, const CallGraph& graph, const std::vector<uint32_t>& components, uint32_t caller)
	{
		std::vector<MarmotIRSite> sites;
		const MarmotIRFunction& function = module.m_functions[caller];
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MarmotIRInstruction>& instructions = function.m_blocks[block].m_instructions;
			for (uint32_t position = 0u; position < instructions.size(); position += 1u)
			{
				if (IsInlinable(module, graph, components, caller, instructions[position]))
				{
					sites.push_back(MarmotIRSite{ block, position });
				}
			}
		}
		// From the last site back, so that inlining one moves no site not yet
		// inlined.
		bool changed = false;
		for (const MarmotIRSite& site : sites | std::views::reverse)
		{
			if (MarmotIRAnalysis::Size(module.m_functions[caller]) > s_max_caller_size)
			{
				break;
			}
			InlineAt(module, caller, site);
			changed = true;
		}
		return changed;
	}
}

bool RunInlining(MarmotIRModule& module)
{
	const CallGraph graph(module);
	const auto [order, components] = graph.Components();
	bool changed = false;
	for (const uint32_t caller : order)
	{
		changed = InlineInto(module, graph, components, caller) || changed;
	}
	return changed;
}
