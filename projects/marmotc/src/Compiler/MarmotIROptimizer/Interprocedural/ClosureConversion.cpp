#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRModuleFacts.h"
#include "Compiler/MarmotIROptimizer/MarmotIRPasses.h"

#include <ranges>
#include <set>
#include <utility>

namespace
{
	bool IsClosureCall(const MarmotIRInstruction& instruction)
	{
		return instruction.m_op == MarmotIROp::CallValue || (instruction.m_op == MarmotIROp::TailCall && std::holds_alternative<std::monostate>(instruction.m_immediate));
	}

	// Every use of `closure` is as what a call calls, or as the closure a
	// BindCaptures binds itself into.
	bool IsOnlyCalled(const MarmotIRFunction& function, MarmotIRValueId closure)
	{
		for (const MarmotIRBlock& block : function.m_blocks)
		{
			for (const MarmotIRInstruction& instruction : block.m_instructions)
			{
				const bool binds_itself = instruction.m_op == MarmotIROp::BindCaptures && instruction.m_operands[0u] == closure && instruction.m_operands[1u] == closure;
				if (binds_itself)
				{
					continue;
				}
				const size_t first_argument = IsClosureCall(instruction) ? 1u : 0u;
				const bool used = std::ranges::contains(instruction.m_operands | std::views::drop(first_argument), closure)
					|| std::ranges::any_of(instruction.m_successors, [closure](const MarmotIRSuccessor& successor) { return std::ranges::contains(successor.m_arguments, closure); });
				if (used)
				{
					return false;
				}
			}
		}
		return true;
	}

	// A call of `closure` becomes a direct call of `function`, with `extra`
	// after its arguments.
	void CallDirectly(MarmotIRFunction& caller, MarmotIRValueId closure, MarmotIRFunctionId function, const std::vector<MarmotIRValueId>& extra)
	{
		for (MarmotIRBlock& block : caller.m_blocks)
		{
			for (MarmotIRInstruction& instruction : block.m_instructions)
			{
				if (!IsClosureCall(instruction) || instruction.m_operands.front() != closure)
				{
					continue;
				}
				instruction.m_operands.erase(instruction.m_operands.begin());
				instruction.m_operands.insert(instruction.m_operands.end(), extra.begin(), extra.end());
				instruction.m_immediate = function;
				if (instruction.m_op == MarmotIROp::CallValue)
				{
					instruction.m_op = MarmotIROp::Call;
				}
			}
		}
	}

	// Leaves a Unit where an instruction was, for anything that used it.
	void MakeUnit(MarmotIRInstruction& instruction)
	{
		instruction = MarmotIRAnalysis::Const(instruction.m_result.value(), MarmotType::MakeLiteralType<MarmotType::UnitType>(), std::monostate{}, instruction.m_line);
	}

	// A capture nothing reads is not captured: each closure is made without
	// it, and what bound it later binds nothing.
	bool DropUnreadCaptures(MarmotIRModule& module)
	{
		std::vector<std::vector<std::optional<uint32_t>>> renumbered(module.m_functions.size());
		bool dropped_any = false;
		for (uint32_t function_index = 0u; function_index < module.m_functions.size(); function_index += 1u)
		{
			MarmotIRFunction& function = module.m_functions[function_index];
			std::vector<bool> read(function.m_capture_types.size(), false);
			for (const MarmotIRBlock& block : function.m_blocks)
			{
				for (const MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (instruction.m_op == MarmotIROp::GetCapture)
					{
						read[std::get<MarmotIRIndex>(instruction.m_immediate).m_value] = true;
					}
				}
			}
			uint32_t next = 0u;
			renumbered[function_index] = read
				| std::views::transform([&next](bool is_read) { return is_read ? std::optional<uint32_t>(next++) : std::nullopt; })
				| std::ranges::to<std::vector>();
			if (std::ranges::contains(read, false))
			{
				dropped_any = true;
				function.m_capture_types = std::views::zip(std::views::iota(0uz), function.m_capture_types)
					| std::views::filter([&read](const auto& capture) { return read[std::get<0>(capture)]; })
					| std::views::transform([](const auto& capture) { return std::get<1>(capture); })
					| std::ranges::to<std::vector>();
			}
		}
		if (!dropped_any)
		{
			return false;
		}

		for (uint32_t function_index = 0u; function_index < module.m_functions.size(); function_index += 1u)
		{
			MarmotIRFunction& function = module.m_functions[function_index];
			const std::vector<std::optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
			std::vector<MarmotIRInstruction*> binds;
			for (MarmotIRBlock& block : function.m_blocks)
			{
				for (MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (instruction.m_op == MarmotIROp::BindCaptures)
					{
						binds.push_back(&instruction);
					}
				}
			}
			// Binds first: each finds its closure's function through the
			// MakeClosure, which does not change.
			for (MarmotIRInstruction* bind : binds)
			{
				const MarmotIRFunctionId bound = std::get<MarmotIRFunctionId>(MarmotIRAnalysis::Definition(function, sites, bind->m_operands[0u])->m_immediate);
				const std::optional<uint32_t> capture = renumbered[bound.m_index][std::get<MarmotIRIndex>(bind->m_immediate).m_value];
				if (capture.has_value())
				{
					bind->m_immediate = MarmotIRIndex{ capture.value() };
				}
				else
				{
					MakeUnit(*bind);
				}
			}
			for (MarmotIRBlock& block : function.m_blocks)
			{
				for (MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (instruction.m_op == MarmotIROp::GetCapture)
					{
						instruction.m_immediate = MarmotIRIndex{ renumbered[function_index][std::get<MarmotIRIndex>(instruction.m_immediate).m_value].value() };
					}
					else if (instruction.m_op == MarmotIROp::MakeClosure)
					{
						const std::vector<std::optional<uint32_t>>& captures = renumbered[std::get<MarmotIRFunctionId>(instruction.m_immediate).m_index];
						instruction.m_operands = std::views::zip(std::views::iota(0uz), instruction.m_operands)
							| std::views::filter([&captures](const auto& operand) { return captures[std::get<0>(operand)].has_value(); })
							| std::views::transform([](const auto& operand) { return std::get<1>(operand); })
							| std::ranges::to<std::vector>();
					}
				}
			}
		}
		return true;
	}

	struct Creation
	{
		uint32_t m_maker;
		MarmotIRValueId m_closure;
		std::vector<MarmotIRValueId> m_captures;
	};

	// A function whose every closure is only ever called takes its captures
	// as parameters instead, after its own: each call passes the ones its
	// closure was made with, and a call through a capture that holds the
	// closure itself passes the function's own.
	bool LiftClosures(MarmotIRModule& module)
	{
		const MarmotIRModuleFacts facts(module);
		bool changed = false;
		std::vector<std::vector<Creation>> creations(module.m_functions.size());
		std::vector<bool> is_liftable(module.m_functions.size(), true);
		for (uint32_t maker = 0u; maker < module.m_functions.size(); maker += 1u)
		{
			const MarmotIRFunction& function = module.m_functions[maker];
			for (const MarmotIRBlock& block : function.m_blocks)
			{
				for (const MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (instruction.m_op == MarmotIROp::MakeClosure)
					{
						const uint32_t made = std::get<MarmotIRFunctionId>(instruction.m_immediate).m_index;
						creations[made].push_back(Creation{ maker, instruction.m_result.value(), instruction.m_operands });
						is_liftable[made] = is_liftable[made] && IsOnlyCalled(function, instruction.m_result.value());
					}
				}
			}
		}

		// A lifted function that makes closures of another passes its new
		// parameters where it passed its captures.
		std::vector<std::vector<std::optional<MarmotIRValueId>>> lifted_captures(module.m_functions.size());
		for (uint32_t lifted = 0u; lifted < module.m_functions.size(); lifted += 1u)
		{
			MarmotIRFunction& function = module.m_functions[lifted];
			const size_t capture_count = function.m_capture_types.size();
			if (capture_count == 0u || creations[lifted].empty() || !is_liftable[lifted])
			{
				continue;
			}
			const MarmotIRFunctionId lifted_id{ lifted };
			const size_t passed = static_cast<size_t>(std::ranges::count_if(std::views::iota(0uz, capture_count), [&](size_t capture) { return !facts.IsSelfCapture(lifted_id, static_cast<uint32_t>(capture)); }));
			const bool self_captures_last = std::ranges::all_of(std::views::iota(passed, capture_count), [&](size_t capture) { return facts.IsSelfCapture(lifted_id, static_cast<uint32_t>(capture)); });
			const bool made_with_passed = std::ranges::all_of(creations[lifted], [passed, lifted](const Creation& creation) { return creation.m_captures.size() == passed && creation.m_maker != lifted; });
			if (!self_captures_last || !made_with_passed)
			{
				continue;
			}

			// The captures that hold the closure itself may only be called.
			std::vector<std::optional<MarmotIRValueId>> replacements(function.m_values.size());
			std::vector<MarmotIRValueId> self_values;
			std::vector<MarmotIRValueId> parameters;
			// Named for what the first closure captured, as the reader knows it.
			const Creation& first = creations[lifted].front();
			for (size_t capture = 0u; capture < passed; capture += 1u)
			{
				const std::string& name = module.m_functions[first.m_maker].Value(first.m_captures[capture]).m_name;
				parameters.push_back(MarmotIRAnalysis::AddValue(function, function.m_capture_types[capture], name));
			}
			bool self_only_called = true;
			for (MarmotIRBlock& block : function.m_blocks)
			{
				for (MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (instruction.m_op != MarmotIROp::GetCapture)
					{
						continue;
					}
					const uint32_t capture = std::get<MarmotIRIndex>(instruction.m_immediate).m_value;
					if (capture < passed)
					{
						replacements[instruction.m_result->m_index] = parameters[capture];
						continue;
					}
					self_values.push_back(instruction.m_result.value());
					self_only_called = self_only_called && IsOnlyCalled(function, instruction.m_result.value());
				}
			}
			if (!self_only_called)
			{
				function.m_values.erase(function.m_values.end() - static_cast<std::ptrdiff_t>(passed), function.m_values.end());
				continue;
			}

			MarmotIRAnalysis::ReplaceUses(function, replacements);
			changed = true;
			lifted_captures[lifted] = std::move(replacements);
			for (const MarmotIRValueId self : self_values)
			{
				CallDirectly(function, self, lifted_id, parameters);
			}
			for (MarmotIRBlock& block : function.m_blocks)
			{
				std::erase_if(block.m_instructions, [](const MarmotIRInstruction& instruction) { return instruction.m_op == MarmotIROp::GetCapture; });
			}
			std::vector<MarmotIRValueId>& entry_parameters = function.Block(MarmotIRFunction::s_entry_block).m_parameters;
			entry_parameters.insert(entry_parameters.end(), parameters.begin(), parameters.end());
			function.m_capture_types.clear();

			for (const Creation& creation : creations[lifted])
			{
				MarmotIRFunction& maker = module.m_functions[creation.m_maker];
				const std::vector<std::optional<MarmotIRValueId>>& maker_captures = lifted_captures[creation.m_maker];
				const std::vector<MarmotIRValueId> captures = creation.m_captures
					| std::views::transform([&maker_captures](MarmotIRValueId capture)
					{
						return capture.m_index < maker_captures.size() && maker_captures[capture.m_index].has_value() ? maker_captures[capture.m_index].value() : capture;
					})
					| std::ranges::to<std::vector>();
				CallDirectly(maker, creation.m_closure, lifted_id, captures);
				for (MarmotIRBlock& block : maker.m_blocks)
				{
					for (MarmotIRInstruction& instruction : block.m_instructions)
					{
						const bool binds_it = instruction.m_op == MarmotIROp::BindCaptures && instruction.m_operands[0u] == creation.m_closure;
						if (binds_it)
						{
							MakeUnit(instruction);
						}
					}
					std::erase_if(block.m_instructions, [&creation](const MarmotIRInstruction& instruction) { return instruction.m_result == creation.m_closure; });
				}
			}
		}
		return changed;
	}

	// A call of a closure made in the same function, of a function with no
	// captures, calls that function.
	bool CallKnownFunctions(MarmotIRModule& module)
	{
		bool changed = false;
		for (MarmotIRFunction& function : module.m_functions)
		{
			const std::vector<std::optional<MarmotIRSite>> sites = MarmotIRAnalysis::DefinitionSites(function);
			std::set<std::pair<uint32_t, uint32_t>> known;
			for (const MarmotIRBlock& block : function.m_blocks)
			{
				for (const MarmotIRInstruction& instruction : block.m_instructions)
				{
					if (!IsClosureCall(instruction))
					{
						continue;
					}
					const MarmotIRInstruction* closure = MarmotIRAnalysis::Definition(function, sites, instruction.m_operands.front());
					if (closure != nullptr && closure->m_op == MarmotIROp::MakeClosure && module.Function(std::get<MarmotIRFunctionId>(closure->m_immediate)).m_capture_types.empty())
					{
						known.emplace(instruction.m_operands.front().m_index, std::get<MarmotIRFunctionId>(closure->m_immediate).m_index);
					}
				}
			}
			for (const auto& [closure, callee] : known)
			{
				CallDirectly(function, MarmotIRValueId{ closure }, MarmotIRFunctionId{ callee }, {});
				changed = true;
			}
		}
		return changed;
	}
}

bool RunClosureConversion(MarmotIRModule& module)
{
	if (!MarmotIRModuleFacts(module).BindsOnlyKnownClosures())
	{
		return false;
	}
	bool changed = DropUnreadCaptures(module);
	changed = LiftClosures(module) || changed;
	return CallKnownFunctions(module) || changed;
}
