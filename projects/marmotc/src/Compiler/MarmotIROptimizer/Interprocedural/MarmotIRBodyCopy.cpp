#include "MarmotIRBodyCopy.h"
#include "Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.h"
#include "Support/Optional.h"

#include <ranges>
#include <utility>

MarmotIRBlockId MarmotIRCopiedBody::Entry() const
{
	return MarmotIRBlockId{ m_first };
}

namespace MarmotIRBodyCopy
{
	MarmotIRCopiedBody Copy(const MarmotIRFunction& source, MarmotIRFunction& target, uint32_t at, MarmotCompiler::Optional<int> line)
	{
		std::vector<MarmotCompiler::Optional<MarmotIRValueId>> renamed(source.m_values.size());
		const auto rename = [&](MarmotIRValueId value)
		{
			if (!renamed[value.m_index].HasValue())
			{
				const MarmotIRValue& original = source.Value(value);
				renamed[value.m_index] = MarmotIRAnalysis::AddValue(target, original.m_type, original.m_name);
			}
			return renamed[value.m_index].Value();
		};

		const uint32_t count = static_cast<uint32_t>(source.m_blocks.size());
		// A block being split has no terminator yet.
		for (MarmotIRBlock& block : target.m_blocks | std::views::filter([](const MarmotIRBlock& block) { return !block.m_instructions.empty(); }))
		{
			for (MarmotIRSuccessor& successor : block.m_instructions.back().m_successors)
			{
				if (successor.m_block.m_index >= at)
				{
					successor.m_block.m_index += count;
				}
			}
		}

		std::vector<MarmotIRBlock> copies(count);
		for (uint32_t block = 0u; block < count; block += 1u)
		{
			const MarmotIRBlock& original = source.m_blocks[block];
			MarmotIRBlock& copy = copies[block];
			copy.m_parameters = original.m_parameters | std::views::transform(rename) | std::ranges::to<std::vector>();
			for (const MarmotIRInstruction& instruction : original.m_instructions)
			{
				MarmotIRInstruction copied = instruction;
				ForEachMarmotIRUse(copied,[&rename](MarmotIRValueId& value) { value = rename(value); });
				if (copied.m_result.HasValue())
				{
					copied.m_result = rename(copied.m_result.Value());
				}
				for (MarmotIRSuccessor& successor : copied.m_successors)
				{
					successor.m_block.m_index += at;
				}
				copied.m_line = line.ValueOr(copied.m_line);
				copy.m_instructions.push_back(std::move(copied));
			}
		}
		target.m_blocks.insert(target.m_blocks.begin() + at, std::make_move_iterator(copies.begin()), std::make_move_iterator(copies.end()));
		return MarmotIRCopiedBody{ at, count };
	}
}
