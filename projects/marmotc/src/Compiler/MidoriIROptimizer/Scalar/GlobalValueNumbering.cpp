#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"
#include "Compiler/MidoriIROptimizer/MidoriIRPasses.h"

#include <bit>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace
{
	bool IsCommutative(MidoriIROp op)
	{
		using enum MidoriIROp;
		switch (op)
		{
		case AddInt:
		case AddByte:
		case AddWord:
		case MulInt:
		case MulByte:
		case MulWord:
		case BitAndInt:
		case BitAndByte:
		case BitAndWord:
		case BitOrInt:
		case BitOrByte:
		case BitOrWord:
		case BitXorInt:
		case BitXorByte:
		case BitXorWord:
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

	// Scalar constants may share a value: the backend still pushes them at
	// each use. Text constants allocate, so each keeps its own identity.
	bool IsNumbered(const MidoriIRInstruction& instruction)
	{
		if (!instruction.m_result.has_value())
		{
			return false;
		}
		if (instruction.m_op == MidoriIROp::Const)
		{
			return !std::holds_alternative<std::string>(instruction.m_immediate);
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

	void CombineHash(size_t& hash, size_t value)
	{
		hash ^= value + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
	}

	struct ValueKey
	{
		MidoriIROp m_op;
		MidoriIRImmediate m_immediate;
		std::vector<MidoriIRValueId> m_operands;

		ValueKey(MidoriIROp op, MidoriIRImmediate immediate, std::vector<MidoriIRValueId> operands = {})
			: m_op(op),
			m_immediate(std::move(immediate)),
			m_operands(std::move(operands))
		{
			if (IsCommutative(op))
			{
				std::ranges::sort(m_operands);
			}
		}

		explicit ValueKey(const MidoriIRInstruction& instruction)
			: ValueKey(instruction.m_op, instruction.m_immediate, instruction.m_operands)
		{
		}

		bool operator==(const ValueKey& other) const
		{
			if (m_op != other.m_op || m_operands != other.m_operands)
			{
				return false;
			}
			if (std::holds_alternative<double>(m_immediate) && std::holds_alternative<double>(other.m_immediate))
			{
				return std::bit_cast<uint64_t>(std::get<double>(m_immediate)) == std::bit_cast<uint64_t>(std::get<double>(other.m_immediate));
			}
			return m_immediate == other.m_immediate;
		}
	};

	size_t ImmediateHash(const MidoriIRImmediate& immediate)
	{
		size_t hash = immediate.index();
		std::visit([&hash](const auto& value)
		{
			using Immediate = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<Immediate, std::monostate>)
			{
			}
			else if constexpr (std::is_same_v<Immediate, int64_t> || std::is_same_v<Immediate, bool> || std::is_same_v<Immediate, std::string>)
			{
				CombineHash(hash, std::hash<Immediate>()(value));
			}
			else if constexpr (std::is_same_v<Immediate, double>)
			{
				CombineHash(hash, std::hash<uint64_t>()(std::bit_cast<uint64_t>(value)));
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRByte> || std::is_same_v<Immediate, MidoriIRWord> || std::is_same_v<Immediate, MidoriIRIndex> || std::is_same_v<Immediate, MidoriIRTag> || std::is_same_v<Immediate, MidoriIRGlobalSlot>)
			{
				CombineHash(hash, std::hash<decltype(value.m_value)>()(value.m_value));
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRUnionField>)
			{
				CombineHash(hash, std::hash<int>()(value.m_tag));
				CombineHash(hash, std::hash<uint32_t>()(value.m_index));
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRFunctionId>)
			{
				CombineHash(hash, std::hash<uint32_t>()(value.m_index));
			}
			else if constexpr (std::is_same_v<Immediate, MidoriIRForeign>)
			{
				CombineHash(hash, std::hash<std::string>()(value.m_name));
			}
			else
			{
				CombineHash(hash, std::hash<int>()(value.m_ok));
				CombineHash(hash, std::hash<int>()(value.m_err));
				CombineHash(hash, std::hash<int>()(value.m_cancelled));
				CombineHash(hash, std::hash<int>()(value.m_failed));
			}
		}, immediate);
		return hash;
	}

	struct ValueKeyHash
	{
		size_t operator()(const ValueKey& key) const
		{
			size_t hash = static_cast<size_t>(key.m_op);
			CombineHash(hash, ImmediateHash(key.m_immediate));
			for (const MidoriIRValueId operand : key.m_operands)
			{
				CombineHash(hash, std::hash<uint32_t>()(operand.m_index));
			}
			return hash;
		}
	};

	struct NumberingScope
	{
		uint32_t m_block;
		std::vector<ValueKey> m_added;
		std::optional<MidoriIRValueId> m_condition;
		std::optional<bool> m_previous_condition;

		explicit NumberingScope(uint32_t block)
			: m_block(block)
		{
		}
	};

	// Walks the dominator tree, each block seeing what its dominators
	// computed. A global's definition also says what reading it gives.
	bool NumberValues(MidoriIRFunction& function)
	{
		const MidoriIRControlFlow control_flow(function);
		const std::vector<std::vector<uint32_t>> children = control_flow.DominatorTree();
		std::vector<std::optional<MidoriIRValueId>> replacements(function.m_values.size());
		std::unordered_map<ValueKey, MidoriIRValueId, ValueKeyHash> available;
		available.reserve(function.m_values.size());
		std::vector<std::optional<bool>> conditions(function.m_values.size());
		bool changed = false;
		const auto resolve = [&replacements](MidoriIRValueId value)
		{
			while (replacements[value.m_index].has_value())
			{
				value = replacements[value.m_index].value();
			}
			return value;
		};

		std::vector<NumberingScope> stack;
		stack.emplace_back(MidoriIRFunction::s_entry_block.m_index);
		std::vector<size_t> next_child(function.m_blocks.size(), 0u);
		std::vector<bool> entered(function.m_blocks.size(), false);
		while (!stack.empty())
		{
			NumberingScope& scope = stack.back();
			const uint32_t block = scope.m_block;
			if (!entered[block])
			{
				entered[block] = true;
				const std::vector<uint32_t>& predecessors = control_flow.Predecessors(block);
				if (block != MidoriIRFunction::s_entry_block.m_index && predecessors.size() == 1u)
				{
					const MidoriIRInstruction& incoming = function.m_blocks[predecessors.front()].m_instructions.back();
					if (incoming.m_op == MidoriIROp::Branch)
					{
						const bool from_true = incoming.m_successors[0u].m_block.m_index == block;
						const bool from_false = incoming.m_successors[1u].m_block.m_index == block;
						// Two edges from one branch may meet at a parameterized block.
						if (from_true != from_false)
						{
							const MidoriIRValueId condition = resolve(incoming.m_operands.front());
							scope.m_condition = condition;
							scope.m_previous_condition = conditions[condition.m_index];
							conditions[condition.m_index] = from_true;
						}
					}
				}
				for (MidoriIRInstruction& instruction : function.m_blocks[block].m_instructions)
				{
					MidoriIRAnalysis::ForEachUse(instruction, [&resolve](MidoriIRValueId& value) { value = resolve(value); });
					if (instruction.m_op == MidoriIROp::Branch && conditions[instruction.m_operands.front().m_index].has_value())
					{
						const bool condition = conditions[instruction.m_operands.front().m_index].value();
						MidoriIRSuccessor taken = std::move(instruction.m_successors[condition ? 0u : 1u]);
						instruction.m_op = MidoriIROp::Jump;
						instruction.m_operands.clear();
						instruction.m_successors.clear();
						instruction.m_successors.push_back(std::move(taken));
						changed = true;
					}
					if (instruction.m_op == MidoriIROp::GlobalDefine)
					{
						ValueKey key(MidoriIROp::GlobalGet, instruction.m_immediate);
						if (available.emplace(key, instruction.m_operands.front()).second)
						{
							scope.m_added.push_back(std::move(key));
						}
						continue;
					}
					if (!IsNumbered(instruction))
					{
						continue;
					}
					ValueKey key(instruction);
					const std::unordered_map<ValueKey, MidoriIRValueId, ValueKeyHash>::const_iterator existing = available.find(key);
					if (existing != available.end())
					{
						replacements[instruction.m_result->m_index] = existing->second;
						continue;
					}
					available.emplace(key, instruction.m_result.value());
					scope.m_added.push_back(std::move(key));
				}
			}
			if (next_child[block] < children[block].size())
			{
				const uint32_t child = children[block][next_child[block]];
				next_child[block] += 1u;
				stack.emplace_back(child);
				continue;
			}
			for (const ValueKey& key : scope.m_added)
			{
				available.erase(key);
			}
			if (scope.m_condition.has_value())
			{
				conditions[scope.m_condition->m_index] = scope.m_previous_condition;
			}
			stack.pop_back();
		}
		MidoriIRAnalysis::ReplaceUses(function, replacements);
		for (MidoriIRBlock& block : function.m_blocks)
		{
			const size_t before = block.m_instructions.size();
			// A dominating computation already succeeded with these operands,
			// so a duplicate fault need not run again either.
			std::erase_if(block.m_instructions, [&replacements](const MidoriIRInstruction& instruction)
			{
				return instruction.m_result.has_value() && replacements[instruction.m_result->m_index].has_value();
			});
			changed = changed || block.m_instructions.size() != before;
		}
		return changed;
	}
}

std::string_view GlobalValueNumberingPass::Name() const
{
	return "GlobalValueNumbering";
}

bool GlobalValueNumberingPass::Run(MidoriIRModule& module) const
{
	return MidoriIRAnalysis::TransformFunctions(module, NumberValues);
}
