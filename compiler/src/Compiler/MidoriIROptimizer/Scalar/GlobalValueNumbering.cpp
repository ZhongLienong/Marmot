#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <format>
#include <map>
#include <ranges>
#include <utility>

namespace
{
	bool IsCommutative(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case AddInt:
		case MulInt:
		case BitAndInt:
		case BitOrInt:
		case BitXorInt:
		case EqInt:
		case NeInt:
		case EqBool:
		case NeBool:
		case EqByte:
		case NeByte:
		case EqWord:
		case NeWord:
			return true;
		default:
			return false;
		}
	}

	// Whether the value depends on nothing but the operands and immediate. A
	// constant is left alone: the backend pushes it where it is used, and a
	// Text one is a fresh allocation each time. A fault is kept: the one
	// that dominates ran first, so this one cannot fail.
	bool IsNumbered(const MidoriIRInstruction& instruction)
	{
		if (instruction.m_op == MidoriIROp::Const || !instruction.m_result.has_value())
		{
			return false;
		}
		switch (instruction.m_effect.m_kind)
		{
		case MidoriIREffectKind::Pure:
		case MidoriIREffectKind::Fault:
			return instruction.m_op != MidoriIROp::TextToInt && instruction.m_op != MidoriIROp::TextToFloat;
		case MidoriIREffectKind::Read:
			return MidoriIRAnalysis::IsStableRead(instruction);
		case MidoriIREffectKind::Alloc:
		case MidoriIREffectKind::Io:
		case MidoriIREffectKind::Call:
			return false;
		}
		std::unreachable();
	}

	std::string ImmediateKey(const MidoriIRImmediate& immediate)
	{
		return std::visit([](const auto& value) -> std::string
		{
			using Immediate = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<Immediate, std::monostate>)
			{
				return "-";
			}
			else if constexpr (std::is_same_v<Immediate, int64_t> || std::is_same_v<Immediate, bool>)
			{
				return std::format("{}", value);
			}
			else if constexpr (std::is_same_v<Immediate, double>)
			{
				return std::format("{}", std::bit_cast<uint64_t>(value));
			}
			else if constexpr (std::is_same_v<Immediate, std::string>)
			{
				return value;
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRByte> || std::is_same_v<Immediate, MidoriIRWord> || std::is_same_v<Immediate, MidoriIRIndex> || std::is_same_v<Immediate, MidoriIRTag> || std::is_same_v<Immediate, MidoriIRGlobalSlot>)
			{
				return std::format("{}", value.m_value);
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRUnionField>)
			{
				return std::format("{}.{}", value.m_tag, value.m_index);
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRFunctionId>)
			{
				return std::format("{}", value.m_index);
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRForeign>)
			{
				return value.m_name;
			}
			else
			{
				return std::format("{}.{}.{}.{}", value.m_ok, value.m_err, value.m_cancelled, value.m_failed);
			}
		}, immediate);
	}

	std::string Key(const MidoriIRInstruction& instruction)
	{
		std::vector<uint32_t> operands = instruction.m_operands
			| std::views::transform([](MidoriIRValueId value) { return value.m_index; })
			| std::ranges::to<std::vector>();
		if (IsCommutative(instruction.m_op))
		{
			std::ranges::sort(operands);
		}
		std::string key = std::format("{}:{}:{}", static_cast<int>(instruction.m_op), instruction.m_immediate.index(), ImmediateKey(instruction.m_immediate));
		for (const uint32_t operand : operands)
		{
			key += std::format(",{}", operand);
		}
		return key;
	}

	std::string GlobalKey(MidoriIRGlobalSlot slot)
	{
		return Key(MidoriIRInstruction(MidoriIROp::GlobalGet, MidoriIRValueId{ 0u }, MidoriType::MakeLiteralType<MidoriType::UnitType>(), {}, slot, {}, 0));
	}

	// Walks the dominator tree, each block seeing what its dominators
	// computed. A global's definition also says what reading it gives.
	void NumberValues(MidoriIRFunction& function)
	{
		const MidoriIRControlFlow control_flow(function);
		const std::vector<std::vector<uint32_t>> children = control_flow.DominatorTree();
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		std::map<std::string, MidoriIRValueId> available;
		const auto resolve = [&replacements](MidoriIRValueId value)
		{
			while (replacements[value.m_index].has_value())
			{
				value = replacements[value.m_index].value();
			}
			return value;
		};

		std::vector<std::pair<uint32_t, std::vector<std::string>>> stack = { { MidoriIRFunction::s_entry_block.m_index, {} } };
		std::vector<size_t> next_child(function.m_blocks.size(), 0u);
		std::vector<bool> entered(function.m_blocks.size(), false);
		while (!stack.empty())
		{
			auto& [block, added] = stack.back();
			if (!entered[block])
			{
				entered[block] = true;
				for (MidoriIRInstruction& instruction : function.m_blocks[block].m_instructions)
				{
					MidoriIRAnalysis::ForEachUse(instruction, [&resolve](MidoriIRValueId& value) { value = resolve(value); });
					if (instruction.m_op == MidoriIROp::GlobalDefine)
					{
						const std::string key = GlobalKey(std::get<MidoriIRGlobalSlot>(instruction.m_immediate));
						if (available.emplace(key, instruction.m_operands.front()).second)
						{
							added.push_back(key);
						}
						continue;
					}
					if (!IsNumbered(instruction))
					{
						continue;
					}
					const std::string key = Key(instruction);
					const std::map<std::string, MidoriIRValueId>::const_iterator existing = available.find(key);
					if (existing != available.end())
					{
						replacements[instruction.m_result->m_index] = existing->second;
						continue;
					}
					available.emplace(key, instruction.m_result.value());
					added.push_back(key);
				}
			}
			if (next_child[block] < children[block].size())
			{
				const uint32_t child = children[block][next_child[block]];
				next_child[block] += 1u;
				stack.emplace_back(child, std::vector<std::string>{});
				continue;
			}
			for (const std::string& key : added)
			{
				available.erase(key);
			}
			stack.pop_back();
		}
		MidoriIRAnalysis::ReplaceUses(function, replacements);
	}
}

std::string_view GlobalValueNumberingPass::Name() const
{
	return "GlobalValueNumbering";
}

void GlobalValueNumberingPass::Run(MidoriIRModule& module) const
{
	std::ranges::for_each(module.m_functions, NumberValues);
}
