#include "MidoriIRAnalysis.h"

#include <ranges>
#include <utility>

MidoriIRControlFlow::MidoriIRControlFlow(const MidoriIRFunction& function)
	: m_successors(function.m_blocks.size()),
	m_predecessors(function.m_blocks.size()),
	m_order(function.m_blocks.size(), UINT32_MAX),
	m_immediate_dominators(function.m_blocks.size())
{
	for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
	{
		const std::vector<MidoriIRInstruction>& instructions = function.m_blocks[block].m_instructions;
		if (instructions.empty())
		{
			continue;
		}
		for (const MidoriIRSuccessor& successor : instructions.back().m_successors)
		{
			if (std::ranges::find(m_successors[block], successor.m_block.m_index) == m_successors[block].end())
			{
				m_successors[block].push_back(successor.m_block.m_index);
			}
		}
	}

	std::vector<uint32_t> postorder;
	std::vector<bool> visited(function.m_blocks.size(), false);
	std::vector<std::pair<uint32_t, size_t>> stack = { { MidoriIRFunction::s_entry_block.m_index, 0u } };
	visited[MidoriIRFunction::s_entry_block.m_index] = true;
	while (!stack.empty())
	{
		auto& [block, next] = stack.back();
		if (next < m_successors[block].size())
		{
			const uint32_t successor = m_successors[block][next];
			next += 1u;
			if (!visited[successor])
			{
				visited[successor] = true;
				stack.emplace_back(successor, 0u);
			}
			continue;
		}
		postorder.push_back(block);
		stack.pop_back();
	}
	m_reverse_postorder.assign(postorder.rbegin(), postorder.rend());
	for (uint32_t index = 0u; index < m_reverse_postorder.size(); index += 1u)
	{
		m_order[m_reverse_postorder[index]] = index;
	}
	for (const uint32_t block : m_reverse_postorder)
	{
		for (const uint32_t successor : m_successors[block])
		{
			m_predecessors[successor].push_back(block);
		}
	}

	const auto intersect = [this](uint32_t left, uint32_t right)
	{
		while (left != right)
		{
			while (m_order[left] > m_order[right])
			{
				left = m_immediate_dominators[left].value();
			}
			while (m_order[right] > m_order[left])
			{
				right = m_immediate_dominators[right].value();
			}
		}
		return left;
	};

	m_immediate_dominators[MidoriIRFunction::s_entry_block.m_index] = MidoriIRFunction::s_entry_block.m_index;
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const uint32_t block : m_reverse_postorder | std::views::drop(1))
		{
			std::optional<uint32_t> dominator;
			for (const uint32_t predecessor : m_predecessors[block])
			{
				if (m_immediate_dominators[predecessor].has_value())
				{
					dominator = dominator.has_value() ? intersect(predecessor, dominator.value()) : predecessor;
				}
			}
			if (dominator != m_immediate_dominators[block])
			{
				m_immediate_dominators[block] = dominator;
				changed = true;
			}
		}
	}
}

const std::vector<uint32_t>& MidoriIRControlFlow::Successors(uint32_t block) const
{
	return m_successors[block];
}

const std::vector<uint32_t>& MidoriIRControlFlow::Predecessors(uint32_t block) const
{
	return m_predecessors[block];
}

const std::vector<uint32_t>& MidoriIRControlFlow::ReversePostorder() const
{
	return m_reverse_postorder;
}

bool MidoriIRControlFlow::IsReachable(uint32_t block) const
{
	return m_order[block] != UINT32_MAX;
}

bool MidoriIRControlFlow::Dominates(uint32_t dominator, uint32_t block) const
{
	if (!IsReachable(block) || !IsReachable(dominator))
	{
		return false;
	}
	while (block != dominator)
	{
		if (block == MidoriIRFunction::s_entry_block.m_index)
		{
			return false;
		}
		block = m_immediate_dominators[block].value();
	}
	return true;
}

std::vector<std::vector<uint32_t>> MidoriIRControlFlow::DominatorTree() const
{
	std::vector<std::vector<uint32_t>> children(m_successors.size());
	for (const uint32_t block : m_reverse_postorder | std::views::drop(1))
	{
		children[m_immediate_dominators[block].value()].push_back(block);
	}
	return children;
}

namespace MidoriIRAnalysis
{
	std::vector<uint32_t> CountUses(const MidoriIRFunction& function)
	{
		std::vector<uint32_t> uses(function.m_values.size(), 0u);
		for (const MidoriIRBlock& block : function.m_blocks)
		{
			for (const MidoriIRInstruction& instruction : block.m_instructions)
			{
				ForEachUse(instruction, [&uses](MidoriIRValueId value) { uses[value.m_index] += 1u; });
			}
		}
		return uses;
	}

	std::vector<std::optional<MidoriIRSite>> DefinitionSites(const MidoriIRFunction& function)
	{
		std::vector<std::optional<MidoriIRSite>> sites(function.m_values.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MidoriIRInstruction>& instructions = function.m_blocks[block].m_instructions;
			for (uint32_t position = 0u; position < instructions.size(); position += 1u)
			{
				if (instructions[position].m_result.has_value())
				{
					sites[instructions[position].m_result->m_index] = MidoriIRSite{ block, position };
				}
			}
		}
		return sites;
	}

	const MidoriIRInstruction* Definition(const MidoriIRFunction& function, const std::vector<std::optional<MidoriIRSite>>& sites, MidoriIRValueId value)
	{
		const std::optional<MidoriIRSite>& site = sites[value.m_index];
		if (!site.has_value())
		{
			return nullptr;
		}
		return &function.m_blocks[site->m_block].m_instructions[site->m_position];
	}

	void ReplaceUses(MidoriIRFunction& function, const std::vector<std::optional<MidoriIRValueId>>& replacements)
	{
		const auto resolve = [&replacements](MidoriIRValueId value)
		{
			while (value.m_index < replacements.size() && replacements[value.m_index].has_value())
			{
				value = replacements[value.m_index].value();
			}
			return value;
		};
		for (MidoriIRBlock& block : function.m_blocks)
		{
			for (MidoriIRInstruction& instruction : block.m_instructions)
			{
				ForEachUse(instruction, [&resolve](MidoriIRValueId& value) { value = resolve(value); });
			}
		}
	}

	MidoriIRValueId AddValue(MidoriIRFunction& function, std::shared_ptr<MidoriType> type, std::string name)
	{
		function.m_values.emplace_back(std::move(type), std::move(name));
		return MidoriIRValueId{ static_cast<uint32_t>(function.m_values.size() - 1u) };
	}

	MidoriIRBlockId InsertBlock(MidoriIRFunction& function, uint32_t index)
	{
		for (MidoriIRBlock& block : function.m_blocks)
		{
			for (MidoriIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				if (successor.m_block.m_index >= index)
				{
					successor.m_block.m_index += 1u;
				}
			}
		}
		function.m_blocks.emplace(function.m_blocks.begin() + index);
		return MidoriIRBlockId{ index };
	}

	void RemoveUnreachableBlocks(MidoriIRFunction& function)
	{
		const MidoriIRControlFlow control_flow(function);
		std::vector<std::optional<uint32_t>> renumbered(function.m_blocks.size());
		std::vector<MidoriIRBlock> kept;
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			if (control_flow.IsReachable(block))
			{
				renumbered[block] = static_cast<uint32_t>(kept.size());
				kept.push_back(std::move(function.m_blocks[block]));
			}
		}
		for (MidoriIRBlock& block : kept)
		{
			for (MidoriIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				successor.m_block = MidoriIRBlockId{ renumbered[successor.m_block.m_index].value() };
			}
		}
		function.m_blocks = std::move(kept);
	}

	bool IsNever(const std::shared_ptr<MidoriType>& type)
	{
		return type->IsType<MidoriType::NeverType>();
	}

	MidoriIRInstruction Jump(MidoriIRSuccessor target, int line)
	{
		return MidoriIRInstruction(MidoriIROp::Jump, std::nullopt, MidoriType::MakeLiteralType<MidoriType::NeverType>(), {}, {}, { std::move(target) }, line);
	}

	MidoriIRInstruction Unreachable(int line)
	{
		return MidoriIRInstruction(MidoriIROp::Unreachable, std::nullopt, MidoriType::MakeLiteralType<MidoriType::NeverType>(), {}, {}, {}, line);
	}

	size_t Size(const MidoriIRFunction& function)
	{
		size_t size = 0u;
		for (const MidoriIRBlock& block : function.m_blocks)
		{
			size += static_cast<size_t>(std::ranges::count_if(block.m_instructions, [](const MidoriIRInstruction& instruction) { return instruction.m_op != MidoriIROp::Const; }));
		}
		return size;
	}

	bool IsRemovable(const MidoriIRFunction& function, const std::vector<std::optional<MidoriIRSite>>& sites, const MidoriIRInstruction& instruction)
	{
		switch (instruction.m_effect.m_kind)
		{
		case MidoriIREffectKind::Pure:
		case MidoriIREffectKind::Alloc:
		case MidoriIREffectKind::Read:
			return true;
		case MidoriIREffectKind::Fault:
		{
			if (instruction.m_effect.m_fault != MidoriIRFault::DivisionByZero)
			{
				return false;
			}
			const MidoriIRInstruction* divisor = Definition(function, sites, instruction.m_operands[1u]);
			if (divisor == nullptr || divisor->m_op != MidoriIROp::Const)
			{
				return false;
			}
			return std::visit([](const auto& value)
			{
				using Immediate = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<Immediate, int64_t>)
				{
					return value != 0;
				}
				else if constexpr (std::is_same_v<Immediate, MidoriIRByte> || std::is_same_v<Immediate, MidoriIRWord>)
				{
					return value.m_value != 0u;
				}
				else
				{
					return false;
				}
			}, divisor->m_immediate);
		}
		case MidoriIREffectKind::Io:
		case MidoriIREffectKind::Call:
			return false;
		}
		std::unreachable();
	}

	bool IsFrameTransparent(const MidoriIRFunction& function)
	{
		const std::vector<std::optional<MidoriIRSite>> sites = DefinitionSites(function);
		return std::ranges::all_of(function.m_blocks, [&](const MidoriIRBlock& block)
		{
			return std::ranges::all_of(block.m_instructions, [&](const MidoriIRInstruction& instruction)
			{
				return IsMidoriIRTerminator(instruction.m_op) || IsRemovable(function, sites, instruction);
			});
		});
	}

	bool IsStableRead(const MidoriIRInstruction& instruction)
	{
		return instruction.m_op == MidoriIROp::GlobalGet;
	}
}
