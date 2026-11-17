#include "MarmotIRAnalysis.h"

#include <bit>
#include <ranges>
#include <utility>

MarmotIRControlFlow::MarmotIRControlFlow(const MarmotIRFunction& function)
	: m_successors(function.m_blocks.size()),
	m_predecessors(function.m_blocks.size()),
	m_order(function.m_blocks.size(), UINT32_MAX),
	m_immediate_dominators(function.m_blocks.size())
{
	for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
	{
		const std::vector<MarmotIRInstruction>& instructions = function.m_blocks[block].m_instructions;
		if (instructions.empty())
		{
			continue;
		}
		for (const MarmotIRSuccessor& successor : instructions.back().m_successors)
		{
			if (std::ranges::find(m_successors[block], successor.m_block.m_index) == m_successors[block].end())
			{
				m_successors[block].push_back(successor.m_block.m_index);
			}
		}
	}

	std::vector<uint32_t> postorder;
	std::vector<bool> visited(function.m_blocks.size(), false);
	std::vector<std::pair<uint32_t, size_t>> stack = { { MarmotIRFunction::s_entry_block.m_index, 0u } };
	visited[MarmotIRFunction::s_entry_block.m_index] = true;
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

	m_immediate_dominators[MarmotIRFunction::s_entry_block.m_index] = MarmotIRFunction::s_entry_block.m_index;
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

const std::vector<uint32_t>& MarmotIRControlFlow::Successors(uint32_t block) const
{
	return m_successors[block];
}

const std::vector<uint32_t>& MarmotIRControlFlow::Predecessors(uint32_t block) const
{
	return m_predecessors[block];
}

const std::vector<uint32_t>& MarmotIRControlFlow::ReversePostorder() const
{
	return m_reverse_postorder;
}

bool MarmotIRControlFlow::IsReachable(uint32_t block) const
{
	return m_order[block] != UINT32_MAX;
}

bool MarmotIRControlFlow::Dominates(uint32_t dominator, uint32_t block) const
{
	if (!IsReachable(block) || !IsReachable(dominator))
	{
		return false;
	}
	while (block != dominator)
	{
		if (block == MarmotIRFunction::s_entry_block.m_index)
		{
			return false;
		}
		block = m_immediate_dominators[block].value();
	}
	return true;
}

std::vector<std::vector<uint32_t>> MarmotIRControlFlow::DominatorTree() const
{
	std::vector<std::vector<uint32_t>> children(m_successors.size());
	for (const uint32_t block : m_reverse_postorder | std::views::drop(1))
	{
		children[m_immediate_dominators[block].value()].push_back(block);
	}
	return children;
}

namespace MarmotIRAnalysis
{
	std::vector<uint32_t> CountUses(const MarmotIRFunction& function)
	{
		std::vector<uint32_t> uses(function.m_values.size(), 0u);
		for (const MarmotIRBlock& block : function.m_blocks)
		{
			for (const MarmotIRInstruction& instruction : block.m_instructions)
			{
				ForEachMarmotIRUse(instruction, [&uses](MarmotIRValueId value) { uses[value.m_index] += 1u; });
			}
		}
		return uses;
	}

	std::vector<std::vector<MarmotIREdge>> IncomingEdges(const MarmotIRFunction& function)
	{
		std::vector<std::vector<MarmotIREdge>> incoming(function.m_blocks.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MarmotIRSuccessor>& successors = function.m_blocks[block].m_instructions.back().m_successors;
			for (size_t successor = 0u; successor < successors.size(); successor += 1u)
			{
				incoming[successors[successor].m_block.m_index].push_back(MarmotIREdge{ block, successor });
			}
		}
		return incoming;
	}

	MarmotIRSuccessor& SuccessorOf(MarmotIRFunction& function, const MarmotIREdge& edge)
	{
		return function.m_blocks[edge.m_block].m_instructions.back().m_successors[edge.m_successor];
	}

	std::vector<std::optional<MarmotIRSite>> DefinitionSites(const MarmotIRFunction& function)
	{
		std::vector<std::optional<MarmotIRSite>> sites(function.m_values.size());
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			const std::vector<MarmotIRInstruction>& instructions = function.m_blocks[block].m_instructions;
			for (uint32_t position = 0u; position < instructions.size(); position += 1u)
			{
				if (instructions[position].m_result.has_value())
				{
					sites[instructions[position].m_result->m_index] = MarmotIRSite{ block, position };
				}
			}
		}
		return sites;
	}

	const MarmotIRInstruction* Definition(const MarmotIRFunction& function, const std::vector<std::optional<MarmotIRSite>>& sites, MarmotIRValueId value)
	{
		const std::optional<MarmotIRSite>& site = sites[value.m_index];
		if (!site.has_value())
		{
			return nullptr;
		}
		return &function.m_blocks[site->m_block].m_instructions[site->m_position];
	}

	void ReplaceUses(MarmotIRFunction& function, const std::vector<std::optional<MarmotIRValueId>>& replacements)
	{
		const auto resolve = [&replacements](MarmotIRValueId value)
		{
			while (value.m_index < replacements.size() && replacements[value.m_index].has_value())
			{
				value = replacements[value.m_index].value();
			}
			return value;
		};
		for (MarmotIRBlock& block : function.m_blocks)
		{
			for (MarmotIRInstruction& instruction : block.m_instructions)
			{
				ForEachMarmotIRUse(instruction, [&resolve](MarmotIRValueId& value) { value = resolve(value); });
			}
		}
	}

	MarmotIRValueId AddValue(MarmotIRFunction& function, std::shared_ptr<MarmotType> type, std::string name)
	{
		function.m_values.emplace_back(std::move(type), std::move(name));
		return MarmotIRValueId{ static_cast<uint32_t>(function.m_values.size() - 1u) };
	}

	MarmotIRBlockId InsertBlock(MarmotIRFunction& function, uint32_t index)
	{
		for (MarmotIRBlock& block : function.m_blocks)
		{
			for (MarmotIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				if (successor.m_block.m_index >= index)
				{
					successor.m_block.m_index += 1u;
				}
			}
		}
		function.m_blocks.emplace(function.m_blocks.begin() + index);
		return MarmotIRBlockId{ index };
	}

	void RemoveUnreachableBlocks(MarmotIRFunction& function)
	{
		const MarmotIRControlFlow control_flow(function);
		std::vector<std::optional<uint32_t>> renumbered(function.m_blocks.size());
		std::vector<MarmotIRBlock> kept;
		for (uint32_t block = 0u; block < function.m_blocks.size(); block += 1u)
		{
			if (control_flow.IsReachable(block))
			{
				renumbered[block] = static_cast<uint32_t>(kept.size());
				kept.push_back(std::move(function.m_blocks[block]));
			}
		}
		for (MarmotIRBlock& block : kept)
		{
			for (MarmotIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				successor.m_block = MarmotIRBlockId{ renumbered[successor.m_block.m_index].value() };
			}
		}
		function.m_blocks = std::move(kept);
	}

	MarmotIRInstruction Const(MarmotIRValueId result, std::shared_ptr<MarmotType> type, MarmotIRImmediate value, int line)
	{
		return MarmotIRInstruction(MarmotIROp::Const, result, std::move(type), {}, std::move(value), {}, line);
	}

	MarmotIRInstruction Jump(MarmotIRSuccessor target, int line)
	{
		return MarmotIRInstruction(MarmotIROp::Jump, std::nullopt, MarmotType::MakeLiteralType<MarmotType::NeverType>(), {}, {}, { std::move(target) }, line);
	}

	MarmotIRInstruction Unreachable(int line)
	{
		return MarmotIRInstruction(MarmotIROp::Unreachable, std::nullopt, MarmotType::MakeLiteralType<MarmotType::NeverType>(), {}, {}, {}, line);
	}

	std::shared_ptr<MarmotType> ConstantType(const MarmotIRImmediate& value, const std::shared_ptr<MarmotType>& fallback)
	{
		return std::visit
		(
			[&fallback](const auto& constant) -> std::shared_ptr<MarmotType>
			{
				using Immediate = std::decay_t<decltype(constant)>;
				if constexpr (std::is_same_v<Immediate, int64_t>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Int);
				}
				else if constexpr (std::is_same_v<Immediate, double>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Float);
				}
				else if constexpr (std::is_same_v<Immediate, bool>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Bool);
				}
				else if constexpr (std::is_same_v<Immediate, std::string>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Text);
				}
				else if constexpr (std::is_same_v<Immediate, MarmotIRByte>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Byte);
				}
				else if constexpr (std::is_same_v<Immediate, MarmotIRWord>)
				{
					return MarmotIRScalarType(MarmotIRScalar::Word);
				}
				else if constexpr (std::is_same_v<Immediate, std::monostate>)
				{
					return MarmotType::MakeLiteralType<MarmotType::UnitType>();
				}
				else
				{
					return fallback;
				}
			},
			value
		);
	}

	bool SameImmediate(const MarmotIRImmediate& left, const MarmotIRImmediate& right)
	{
		if (std::holds_alternative<double>(left) && std::holds_alternative<double>(right))
		{
			return std::bit_cast<uint64_t>(std::get<double>(left)) == std::bit_cast<uint64_t>(std::get<double>(right));
		}
		return left == right;
	}

	size_t Size(const MarmotIRFunction& function)
	{
		size_t size = 0u;
		for (const MarmotIRBlock& block : function.m_blocks)
		{
			size += static_cast<size_t>(std::ranges::count_if(block.m_instructions, [](const MarmotIRInstruction& instruction) { return instruction.m_op != MarmotIROp::Const; }));
		}
		return size;
	}

	bool IsRemovable(const MarmotIRFunction& function, const std::vector<std::optional<MarmotIRSite>>& sites, const MarmotIRInstruction& instruction)
	{
		switch (instruction.m_effect.m_kind)
		{
		case MarmotIREffectKind::Pure:
		case MarmotIREffectKind::Alloc:
		case MarmotIREffectKind::Read:
			return true;
		case MarmotIREffectKind::Fault:
		{
			if (instruction.m_effect.m_fault != MarmotIRFault::DivisionByZero)
			{
				return false;
			}
			const MarmotIRInstruction* divisor = Definition(function, sites, instruction.m_operands[1u]);
			if (divisor == nullptr || divisor->m_op != MarmotIROp::Const)
			{
				return false;
			}
			return std::visit
			(
				[](const auto& value)
				{
					using Immediate = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<Immediate, int64_t>)
					{
						return value != 0;
					}
					else if constexpr (std::is_same_v<Immediate, MarmotIRByte> || std::is_same_v<Immediate, MarmotIRWord>)
					{
						return value.m_value != 0u;
					}
					else
					{
						return false;
					}
				},
				divisor->m_immediate
			);
		}
		case MarmotIREffectKind::Io:
		case MarmotIREffectKind::Call:
			return false;
		}
		std::unreachable();
	}

	bool RefineEffects(MarmotIRFunction& function)
	{
		const std::vector<std::optional<MarmotIRSite>> sites = DefinitionSites(function);
		bool changed = false;
		for (MarmotIRBlock& block : function.m_blocks)
		{
			for (MarmotIRInstruction& instruction : block.m_instructions)
			{
				if (instruction.m_effect.m_kind == MarmotIREffectKind::Fault && IsRemovable(function, sites, instruction))
				{
					instruction.m_effect = MarmotIREffect();
					changed = true;
				}
			}
		}
		return changed;
	}

	bool IsFrameTransparent(const MarmotIRFunction& function)
	{
		const std::vector<std::optional<MarmotIRSite>> sites = DefinitionSites(function);
		return std::ranges::all_of(function.m_blocks, [&](const MarmotIRBlock& block)
		{
			return std::ranges::all_of(block.m_instructions, [&](const MarmotIRInstruction& instruction)
			{
				return IsMarmotIRTerminator(instruction.m_op) || IsRemovable(function, sites, instruction);
			});
		});
	}

	bool IsStableRead(const MarmotIRInstruction& instruction)
	{
		return instruction.m_op == MarmotIROp::GlobalGet;
	}
}
