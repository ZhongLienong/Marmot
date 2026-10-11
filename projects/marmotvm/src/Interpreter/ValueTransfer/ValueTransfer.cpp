#include "ValueTransfer.h"

#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <ranges>
#include <span>
#include <vector>

namespace
{
	template<typename... Visitors>
	struct Overloaded : Visitors...
	{
		using Visitors::operator()...;
	};
}

SerializedValue ValueTransfer::Serialize(MidoriValue source, VirtualMachine& source_vm)
{
	SerializePointerMap visited;
	return Serialize(source, source_vm, visited);
}

MidoriValue ValueTransfer::Deserialize(const SerializedValue& source, VirtualMachine& target_vm)
{
	DeserializePointerMap visited;
	return Deserialize(source, target_vm, visited);
}

SerializedValue ValueTransfer::Serialize(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& visited)
{
	MidoriTraceable* pointer = source.GetPointer();
	if (pointer == nullptr || !source_vm.GetGC().Contains(pointer))
	{
		return SerializedValue{ source.GetRawBits(), nullptr };
	}

	return SerializedValue{ 0u, SerializeTraceable(pointer, source_vm, visited) };
}

std::shared_ptr<SerializedObject> ValueTransfer::SerializeTraceable(MidoriTraceable* source, VirtualMachine& source_vm, SerializePointerMap& visited)
{
	SerializePointerMap::iterator visited_it = visited.find(source);
	if (visited_it != visited.end())
	{
		return visited_it->second;
	}

	// Registered before its contents are copied, so a cycle back to it (a
	// recursive local closure captures itself) becomes a shared reference.
	std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>();
	visited.emplace(source, serialized);

	const auto serialize = [&source_vm, &visited](MidoriValue value) -> SerializedValue
	{
		return Serialize(value, source_vm, visited);
	};

	switch (source->GetType())
	{
	case MidoriTraceable::TraceableType::Text:
	{
		serialized->m_data = SerializedObject::Text{ std::string(source->GetTraceable<MidoriText>().View()) };
		break;
	}
	case MidoriTraceable::TraceableType::Array:
	{
		const MidoriArray& array = source->GetTraceable<MidoriArray>();
		serialized->m_data = SerializedObject::Array
		{
			std::views::iota(0, array.GetLength())
				| std::views::transform([&array, &serialize](int index) -> SerializedValue { return serialize(array[index]); })
				| std::ranges::to<std::vector<SerializedValue>>()
		};
		break;
	}
	// A closure is immutable once built (v2 has no assignment), so a structural
	// copy is the same value.
	case MidoriTraceable::TraceableType::Tuple:
	case MidoriTraceable::TraceableType::Struct:
	case MidoriTraceable::TraceableType::Union:
	case MidoriTraceable::TraceableType::Closure:
	{
		const std::span<const MidoriValue> values(source->GetValues(), static_cast<size_t>(source->GetLength()));
		serialized->m_data = SerializedObject::Aggregate
		{
			source->GetType(),
			source->GetIndex(),
			values | std::views::transform(serialize) | std::ranges::to<std::vector<SerializedValue>>()
		};
		break;
	}
	case MidoriTraceable::TraceableType::IntRange:
	{
		const MidoriIntRange& range = source->GetTraceable<MidoriIntRange>();
		serialized->m_data = SerializedObject::IntRange{ range.GetStart(), range.GetEnd(), range.GetStep() };
		break;
	}
	case MidoriTraceable::TraceableType::FloatRange:
	{
		const MidoriFloatRange& range = source->GetTraceable<MidoriFloatRange>();
		serialized->m_data = SerializedObject::FloatRange{ range.GetStart(), range.GetEnd(), range.GetStep() };
		break;
	}
	case MidoriTraceable::TraceableType::MutableCell:
	{
		serialized->m_data = SerializedObject::MutableCell{ serialize(source->GetTraceable<MidoriMutableCell>().m_value) };
		break;
	}
	}

	return serialized;
}

MidoriValue ValueTransfer::Deserialize(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& visited)
{
	if (!source.HasObject())
	{
		return MidoriValue::FromRawBits(source.m_raw_bits);
	}

	return MidoriValue(DeserializeObject(*source.m_object, target_vm, visited));
}

MidoriTraceable* ValueTransfer::DeserializeObject(const SerializedObject& source, VirtualMachine& target_vm, DeserializePointerMap& visited)
{
	DeserializePointerMap::iterator visited_it = visited.find(&source);
	if (visited_it != visited.end())
	{
		return visited_it->second;
	}

	// Registered before its contents are filled in, for the same cycles as above.
	const auto registered = [&source, &visited](MidoriTraceable* transferred) -> MidoriTraceable*
	{
		visited.emplace(&source, transferred);
		return transferred;
	};

	return std::visit
	(
		Overloaded
		{
			[&](const SerializedObject::Text& text) -> MidoriTraceable*
			{
				return registered(target_vm.AllocateTraceable(MidoriText(text.m_text.c_str())));
			},
			[&](const SerializedObject::Array& array) -> MidoriTraceable*
			{
				MidoriTraceable* transferred = registered(target_vm.AllocateTraceable(MidoriArray(static_cast<int>(array.m_elements.size()))));
				MidoriArray& elements = transferred->GetTraceable<MidoriArray>();
				for (size_t index = 0uz; index < array.m_elements.size(); index += 1uz)
				{
					elements[static_cast<int>(index)] = Deserialize(array.m_elements[index], target_vm, visited);
				}
				return transferred;
			},
			[&](const SerializedObject::Aggregate& aggregate) -> MidoriTraceable*
			{
				// A capture-free function goes through the target VM's static closure
				// cache, exactly as MAKE_FUNCTION_WIDE would produce it there.
				if (aggregate.m_type == MidoriTraceable::TraceableType::Closure && aggregate.m_values.empty())
				{
					return registered(target_vm.MakeFunctionValue(aggregate.m_index).GetPointer());
				}

				const std::vector<MidoriValue> placeholders(aggregate.m_values.size());
				MidoriTraceable* transferred = registered(target_vm.AllocateAggregate(aggregate.m_type, placeholders, aggregate.m_index));
				MidoriValue* values = transferred->GetValues();
				for (size_t index = 0uz; index < aggregate.m_values.size(); index += 1uz)
				{
					values[index] = Deserialize(aggregate.m_values[index], target_vm, visited);
				}
				return transferred;
			},
			[&](const SerializedObject::IntRange& range) -> MidoriTraceable*
			{
				return registered(target_vm.AllocateTraceable(MidoriIntRange(range.m_start, range.m_end, range.m_step)));
			},
			[&](const SerializedObject::FloatRange& range) -> MidoriTraceable*
			{
				return registered(target_vm.AllocateTraceable(MidoriFloatRange(range.m_start, range.m_end, range.m_step)));
			},
			[&](const SerializedObject::MutableCell& cell) -> MidoriTraceable*
			{
				MidoriTraceable* transferred = registered(target_vm.AllocateTraceable(MidoriMutableCell(MidoriValue())));
				transferred->GetTraceable<MidoriMutableCell>().m_value = Deserialize(cell.m_value, target_vm, visited);
				return transferred;
			}
		},
		source.m_data
	);
}
