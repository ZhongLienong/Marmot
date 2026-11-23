#include "ValueTransfer.h"

#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <algorithm>
#include <ranges>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	template<typename... Visitors>
	struct Overloaded : Visitors...
	{
		using Visitors::operator()...;
	};

	// Copies values out of one VM's heap, each by the kind the compiler gave it: an
	// array's flag, an aggregate's shape, a global's kind. A pointer
	// that is null, a global not defined yet or a capture not bound yet, crosses
	// as the zero it is. An object gets its index when first
	// reached and is copied later, from a worklist, so a cycle (a recursive local
	// closure captures itself) is an index pointing back. A copied closure brings
	// its procedure's referenced globals. Copying their values can reach more
	// closures, whose globals are copied in turn.
	class PayloadWriter
	{
	public:
		explicit PayloadWriter(VirtualMachine& source_vm);

		std::expected<TransferPayload, std::string_view> Write(MarmotValue value, bool is_pointer) &&;

	private:
		SerializedValue Reference(MarmotValue value, bool is_pointer);

		std::vector<SerializedValue> ReferenceAll(std::span<const MarmotValue> values, std::span<const uint32_t> pointer_fields);

		std::optional<SerializedObject> Copy(MarmotTraceable* source);

		bool CopyPendingObjects();

		void ReachProcedure(int proc_index);

		void ReachGlobal(int global_index);

		VirtualMachine& m_source_vm;
		TransferPayload m_payload;
		std::unordered_map<MarmotTraceable*, MarmotWord> m_object_indices;
		std::vector<std::pair<MarmotTraceable*, size_t>> m_pending_objects;
		std::unordered_set<int> m_reached_procedures;
		std::unordered_set<int> m_reached_globals;
	};

	PayloadWriter::PayloadWriter(VirtualMachine& source_vm)
		: m_source_vm(source_vm)
	{
	}

	std::expected<TransferPayload, std::string_view> PayloadWriter::Write(MarmotValue value, bool is_pointer) &&
	{
		m_payload.m_root = Reference(value, is_pointer);
		if (!CopyPendingObjects())
		{
			return std::unexpected("References cannot cross worker boundaries.");
		}
		return std::move(m_payload);
	}

	SerializedValue PayloadWriter::Reference(MarmotValue value, bool is_pointer)
	{
		if (!is_pointer || value.GetPointer() == nullptr)
		{
			return SerializedValue{ value.GetRawBits(), false };
		}

		const auto [entry, inserted] = m_object_indices.try_emplace(value.GetPointer(), static_cast<MarmotWord>(m_payload.m_objects.size()));
		if (inserted)
		{
			m_payload.m_objects.emplace_back();
			m_pending_objects.emplace_back(value.GetPointer(), m_payload.m_objects.size() - 1uz);
		}
		return SerializedValue{ entry->second, true };
	}

	// The values in order, the fields the shape lists as pointers among them.
	std::vector<SerializedValue> PayloadWriter::ReferenceAll(std::span<const MarmotValue> values, std::span<const uint32_t> pointer_fields)
	{
		std::vector<SerializedValue> references;
		references.reserve(values.size());
		std::span<const uint32_t>::iterator next_pointer = pointer_fields.begin();
		for (uint32_t index = 0u; index < values.size(); index += 1u)
		{
			const bool is_pointer = next_pointer != pointer_fields.end() && *next_pointer == index;
			next_pointer += is_pointer ? 1 : 0;
			references.push_back(Reference(values[index], is_pointer));
		}
		return references;
	}

	bool PayloadWriter::CopyPendingObjects()
	{
		while (!m_pending_objects.empty())
		{
			const auto [source, index] = m_pending_objects.back();
			m_pending_objects.pop_back();
			std::optional<SerializedObject> copy = Copy(source);
			if (!copy.has_value())
			{
				return false;
			}
			m_payload.m_objects[index] = std::move(*copy);
		}
		return true;
	}

	std::optional<SerializedObject> PayloadWriter::Copy(MarmotTraceable* source)
	{
		switch (source->GetType())
		{
		case MarmotTraceable::TraceableType::Text:
		{
			return SerializedObject{ SerializedObject::Text{ std::string(source->GetTraceable<MarmotText>().View()) } };
		}
		case MarmotTraceable::TraceableType::Array:
		{
			const MarmotArray& array = source->GetTraceable<MarmotArray>();
			const bool holds_pointers = source->HoldsPointers();
			return SerializedObject
			{
				SerializedObject::Array
				{
					std::views::iota(0, array.GetLength())
						| std::views::transform([this, &array, holds_pointers](int index) -> SerializedValue { return Reference(array[index], holds_pointers); })
						| std::ranges::to<std::vector<SerializedValue>>(),
					holds_pointers
				}
			};
		}
		case MarmotTraceable::TraceableType::Tuple:
		case MarmotTraceable::TraceableType::Struct:
		case MarmotTraceable::TraceableType::Union:
		case MarmotTraceable::TraceableType::Closure:
		{
			if (source->GetType() == MarmotTraceable::TraceableType::Closure)
			{
				ReachProcedure(source->GetIndex());
			}

			const std::span<const MarmotValue> values(source->GetValues(), static_cast<size_t>(source->GetLength()));
			const VmShape& shape = m_source_vm.GetExecutable().GetShapes()[source->GetShape()];
			return SerializedObject
			{
				SerializedObject::Aggregate
				{
					source->GetType(),
					source->GetIndex(),
					source->GetShape(),
					ReferenceAll(values, shape)
				}
			};
		}
		case MarmotTraceable::TraceableType::IntRange:
		{
			const MarmotIntRange& range = source->GetTraceable<MarmotIntRange>();
			return SerializedObject{ SerializedObject::IntRange{ range.GetStart(), range.GetEnd(), range.GetStep() } };
		}
		case MarmotTraceable::TraceableType::FloatRange:
		{
			const MarmotFloatRange& range = source->GetTraceable<MarmotFloatRange>();
			return SerializedObject{ SerializedObject::FloatRange{ range.GetStart(), range.GetEnd(), range.GetStep() } };
		}
		case MarmotTraceable::TraceableType::MutableCell:
		{
			return std::nullopt;
		}
		}

		std::unreachable();
	}

	void PayloadWriter::ReachProcedure(int proc_index)
	{
		if (m_reached_procedures.insert(proc_index).second)
		{
			for (const uint16_t global : m_source_vm.GetExecutable().GetBytecodeStream(proc_index).GetReferencedGlobals())
			{
				ReachGlobal(global);
			}
		}
	}

	void PayloadWriter::ReachGlobal(int global_index)
	{
		if (m_reached_globals.insert(global_index).second)
		{
			m_payload.m_globals.push_back(SerializedGlobal{ global_index, Reference(m_source_vm.GetGlobalValue(global_index), m_source_vm.GetExecutable().IsGlobalPointer(global_index)) });
		}
	}

	MarmotTraceable* Allocate(const SerializedObject& object, VirtualMachine& target_vm)
	{
		return std::visit
		(
			Overloaded
			{
				[&target_vm](const SerializedObject::Text& text) -> MarmotTraceable*
				{
					return target_vm.AllocateTraceable(MarmotText(text.m_text.c_str()));
				},
				[&target_vm](const SerializedObject::Array& array) -> MarmotTraceable*
				{
					return target_vm.AllocateArray(MarmotArray(static_cast<int>(array.m_elements.size())), array.m_holds_pointers);
				},
				[&target_vm](const SerializedObject::Aggregate& aggregate) -> MarmotTraceable*
				{
					// A capture-free function goes through the target VM's static closure
					// cache, exactly as MAKE_FUNCTION_WIDE would produce it there.
					if (aggregate.m_type == MarmotTraceable::TraceableType::Closure && aggregate.m_values.empty())
					{
						return target_vm.MakeFunctionValue(aggregate.m_index).GetPointer();
					}

					const std::vector<MarmotValue> placeholders(aggregate.m_values.size());
					return target_vm.AllocateAggregate(aggregate.m_type, placeholders, aggregate.m_index, aggregate.m_shape);
				},
				[&target_vm](const SerializedObject::IntRange& range) -> MarmotTraceable*
				{
					return target_vm.AllocateTraceable(MarmotIntRange(range.m_start, range.m_end, range.m_step));
				},
				[&target_vm](const SerializedObject::FloatRange& range) -> MarmotTraceable*
				{
					return target_vm.AllocateTraceable(MarmotFloatRange(range.m_start, range.m_end, range.m_step));
				}
			},
			object.m_data
		);
	}
}

// Most messages are a number or a flag, which need none of the writer's tables.
std::expected<TransferPayload, std::string_view> ValueTransfer::Serialize(MarmotValue value, bool is_pointer, VirtualMachine& source_vm)
{
	if (!is_pointer)
	{
		return TransferPayload{ {}, {}, SerializedValue{ value.GetRawBits(), false } };
	}

	return PayloadWriter(source_vm).Write(value, true);
}

MarmotValue ValueTransfer::Deserialize(const TransferPayload& payload, VirtualMachine& target_vm)
{
	// Every object is allocated before any is filled in, so a reference to any of
	// them, back or forward, is a lookup.
	const std::vector<MarmotTraceable*> objects = payload.m_objects
		| std::views::transform([&target_vm](const SerializedObject& object) -> MarmotTraceable* { return Allocate(object, target_vm); })
		| std::ranges::to<std::vector<MarmotTraceable*>>();
	const auto resolve = [&objects](SerializedValue value) -> MarmotValue
	{
		return value.m_is_object ? MarmotValue(objects[static_cast<size_t>(value.m_bits)]) : MarmotValue::FromRawBits(value.m_bits);
	};

	for (size_t index = 0uz; index < objects.size(); index += 1uz)
	{
		MarmotTraceable* transferred = objects[index];
		std::visit
		(
			Overloaded
			{
				[transferred, &resolve](const SerializedObject::Array& array)
				{
					MarmotArray& elements = transferred->GetTraceable<MarmotArray>();
					for (size_t element = 0uz; element < array.m_elements.size(); element += 1uz)
					{
						elements[static_cast<int>(element)] = resolve(array.m_elements[element]);
					}
				},
				[transferred, &resolve](const SerializedObject::Aggregate& aggregate)
				{
					std::ranges::transform(aggregate.m_values, transferred->GetValues(), resolve);
				},
				[](const auto&)
				{
				}
			},
			payload.m_objects[index].m_data
		);
	}

	for (const SerializedGlobal& global : payload.m_globals)
	{
		target_vm.InstallGlobal(global.m_index, resolve(global.m_value));
	}

	return resolve(payload.m_root);
}
