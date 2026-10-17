#include "MidoriIRBodyCopy.h"
#include "Compiler/MidoriIROptimizer/Analysis/MidoriIRAnalysis.h"

#include <ranges>
#include <utility>

MidoriIRBlockId MidoriIRCopiedBody::Entry() const
{
	return MidoriIRBlockId{ m_first };
}

namespace MidoriIRBodyCopy
{
	MidoriIRCopiedBody Copy(const MidoriIRFunction& source, MidoriIRFunction& target, uint32_t at, std::optional<int> line)
	{
		std::vector<std::optional<MidoriIRValueId>> renamed(source.m_values.size());
		const auto rename = [&](MidoriIRValueId value)
		{
			if (!renamed[value.m_index].has_value())
			{
				const MidoriIRValue& original = source.Value(value);
				renamed[value.m_index] = MidoriIRAnalysis::AddValue(target, original.m_type, original.m_name);
			}
			return renamed[value.m_index].value();
		};

		const uint32_t count = static_cast<uint32_t>(source.m_blocks.size());
		// A block being split has no terminator yet.
		for (MidoriIRBlock& block : target.m_blocks | std::views::filter([](const MidoriIRBlock& block) { return !block.m_instructions.empty(); }))
		{
			for (MidoriIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				if (successor.m_block.m_index >= at)
				{
					successor.m_block.m_index += count;
				}
			}
		}

		std::vector<MidoriIRBlock> copies(count);
		for (uint32_t block = 0u; block < count; block += 1u)
		{
			const MidoriIRBlock& original = source.m_blocks[block];
			MidoriIRBlock& copy = copies[block];
			copy.m_parameters = original.m_parameters | std::views::transform(rename) | std::ranges::to<std::vector>();
			for (const MidoriIRInstruction& instruction : original.m_instructions)
			{
				MidoriIRInstruction copied = instruction;
				ForEachMidoriIRUse(copied,[&rename](MidoriIRValueId& value) { value = rename(value); });
				if (copied.m_result.has_value())
				{
					copied.m_result = rename(copied.m_result.value());
				}
				for (MidoriIRSuccessor& successor : copied.m_successors)
				{
					successor.m_block.m_index += at;
				}
				copied.m_line = line.value_or(copied.m_line);
				copy.m_instructions.push_back(std::move(copied));
			}
		}
		target.m_blocks.insert(target.m_blocks.begin() + at, std::make_move_iterator(copies.begin()), std::make_move_iterator(copies.end()));
		return MidoriIRCopiedBody{ at, count };
	}
}
