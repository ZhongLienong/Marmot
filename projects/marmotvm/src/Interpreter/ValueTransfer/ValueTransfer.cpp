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
	// array's or a cell's flag, an aggregate's shape, a global's kind. A pointer
	// that is null, a global not defined yet or a capture not bound yet, crosses
	// as the zero it is. An object gets its index when first
	// reached and is copied later, from a worklist, so a cycle (a recursive local
	// closure captures itself) is an index pointing back. A copied closure brings
	// its procedure, whose bytecode names the globals it reads and the procedures
	// it calls or makes closures of; those are followed in turn, so the payload
	// carries every global the functions in it can reach, and no other.
	class PayloadWriter
	{
	public:
		explicit PayloadWriter(VirtualMachine& source_vm);

		TransferPayload Write(MidoriValue value, bool is_pointer) &&;

	private:
		SerializedValue Reference(MidoriValue value, bool is_pointer);

		std::vector<SerializedValue> ReferenceAll(std::span<const MidoriValue> values, std::span<const uint32_t> pointer_fields);

		SerializedObject Copy(MidoriTraceable* source);

		void CopyPendingObjects();

		void ReachProcedure(int proc_index);

		void ReachGlobal(int global_index);

		void ScanProcedure(int proc_index);

		VirtualMachine& m_source_vm;
		TransferPayload m_payload;
		std::unordered_map<MidoriTraceable*, MidoriWord> m_object_indices;
		std::vector<std::pair<MidoriTraceable*, size_t>> m_pending_objects;
		std::unordered_set<int> m_reached_procedures;
		std::vector<int> m_pending_procedures;
		std::unordered_set<int> m_reached_globals;
	};

	PayloadWriter::PayloadWriter(VirtualMachine& source_vm)
		: m_source_vm(source_vm)
	{
	}

	TransferPayload PayloadWriter::Write(MidoriValue value, bool is_pointer) &&
	{
		m_payload.m_root = Reference(value, is_pointer);
		CopyPendingObjects();
		while (!m_pending_procedures.empty())
		{
			const int proc_index = m_pending_procedures.back();
			m_pending_procedures.pop_back();
			ScanProcedure(proc_index);
			CopyPendingObjects();
		}

		return std::move(m_payload);
	}

	SerializedValue PayloadWriter::Reference(MidoriValue value, bool is_pointer)
	{
		if (!is_pointer || value.GetPointer() == nullptr)
		{
			return SerializedValue{ value.GetRawBits(), false };
		}

		const auto [entry, inserted] = m_object_indices.try_emplace(value.GetPointer(), static_cast<MidoriWord>(m_payload.m_objects.size()));
		if (inserted)
		{
			m_payload.m_objects.emplace_back();
			m_pending_objects.emplace_back(value.GetPointer(), m_payload.m_objects.size() - 1uz);
		}
		return SerializedValue{ entry->second, true };
	}

	// The values in order, the fields the shape lists as pointers among them.
	std::vector<SerializedValue> PayloadWriter::ReferenceAll(std::span<const MidoriValue> values, std::span<const uint32_t> pointer_fields)
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

	void PayloadWriter::CopyPendingObjects()
	{
		while (!m_pending_objects.empty())
		{
			const auto [source, index] = m_pending_objects.back();
			m_pending_objects.pop_back();
			SerializedObject copy = Copy(source);
			m_payload.m_objects[index] = std::move(copy);
		}
	}

	SerializedObject PayloadWriter::Copy(MidoriTraceable* source)
	{
		switch (source->GetType())
		{
		case MidoriTraceable::TraceableType::Text:
		{
			return SerializedObject{ SerializedObject::Text{ std::string(source->GetTraceable<MidoriText>().View()) } };
		}
		case MidoriTraceable::TraceableType::Array:
		{
			const MidoriArray& array = source->GetTraceable<MidoriArray>();
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
		// A closure is immutable once built (v2 has no assignment), so a structural
		// copy is the same value.
		case MidoriTraceable::TraceableType::Tuple:
		case MidoriTraceable::TraceableType::Struct:
		case MidoriTraceable::TraceableType::Union:
		case MidoriTraceable::TraceableType::Closure:
		{
			if (source->GetType() == MidoriTraceable::TraceableType::Closure)
			{
				ReachProcedure(source->GetIndex());
			}

			const std::span<const MidoriValue> values(source->GetValues(), static_cast<size_t>(source->GetLength()));
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
		case MidoriTraceable::TraceableType::IntRange:
		{
			const MidoriIntRange& range = source->GetTraceable<MidoriIntRange>();
			return SerializedObject{ SerializedObject::IntRange{ range.GetStart(), range.GetEnd(), range.GetStep() } };
		}
		case MidoriTraceable::TraceableType::FloatRange:
		{
			const MidoriFloatRange& range = source->GetTraceable<MidoriFloatRange>();
			return SerializedObject{ SerializedObject::FloatRange{ range.GetStart(), range.GetEnd(), range.GetStep() } };
		}
		case MidoriTraceable::TraceableType::MutableCell:
		{
			return SerializedObject{ SerializedObject::MutableCell{ Reference(source->GetTraceable<MidoriMutableCell>().m_value, source->HoldsPointers()), source->HoldsPointers() } };
		}
		}

		std::unreachable();
	}

	void PayloadWriter::ReachProcedure(int proc_index)
	{
		if (m_reached_procedures.insert(proc_index).second)
		{
			m_pending_procedures.push_back(proc_index);
		}
	}

	void PayloadWriter::ReachGlobal(int global_index)
	{
		if (m_reached_globals.insert(global_index).second)
		{
			m_payload.m_globals.push_back(SerializedGlobal{ global_index, Reference(m_source_vm.GetGlobalValue(global_index), m_source_vm.GetExecutable().IsGlobalPointer(global_index)) });
		}
	}

	// A global's index is written high byte first, a procedure's low byte first.
	void PayloadWriter::ScanProcedure(int proc_index)
	{
		const VmBytecodeStream& bytecode = m_source_vm.GetExecutable().GetBytecodeStream(proc_index);
		const auto operand = [&bytecode](int offset) -> int
		{
			return static_cast<int>(bytecode.ReadByteCode(offset));
		};

		for (int offset = 0; offset < bytecode.GetByteCodeSize(); offset += VmOpCodeTable::Length(bytecode.ReadByteCode(offset)))
		{
			switch (bytecode.ReadByteCode(offset))
			{
			case VmOpCode::GET_GLOBAL_WIDE:
			case VmOpCode::CALL_GLOBAL_WIDE:
			{
				ReachGlobal((operand(offset + 1) << 8) | operand(offset + 2));
				break;
			}
			case VmOpCode::CALL_PROC_WIDE:
			case VmOpCode::MAKE_FUNCTION_WIDE:
			case VmOpCode::MAKE_CLOSURE_OF:
			{
				ReachProcedure(operand(offset + 1) | (operand(offset + 2) << 8));
				break;
			}
			default:
			{
				break;
			}
			}
		}
	}

	MidoriTraceable* Allocate(const SerializedObject& object, VirtualMachine& target_vm)
	{
		return std::visit
		(
			Overloaded
			{
				[&target_vm](const SerializedObject::Text& text) -> MidoriTraceable*
				{
					return target_vm.AllocateTraceable(MidoriText(text.m_text.c_str()));
				},
				[&target_vm](const SerializedObject::Array& array) -> MidoriTraceable*
				{
					return target_vm.AllocateArray(MidoriArray(static_cast<int>(array.m_elements.size())), array.m_holds_pointers);
				},
				[&target_vm](const SerializedObject::Aggregate& aggregate) -> MidoriTraceable*
				{
					// A capture-free function goes through the target VM's static closure
					// cache, exactly as MAKE_FUNCTION_WIDE would produce it there.
					if (aggregate.m_type == MidoriTraceable::TraceableType::Closure && aggregate.m_values.empty())
					{
						return target_vm.MakeFunctionValue(aggregate.m_index).GetPointer();
					}

					const std::vector<MidoriValue> placeholders(aggregate.m_values.size());
					return target_vm.AllocateAggregate(aggregate.m_type, placeholders, aggregate.m_index, aggregate.m_shape);
				},
				[&target_vm](const SerializedObject::IntRange& range) -> MidoriTraceable*
				{
					return target_vm.AllocateTraceable(MidoriIntRange(range.m_start, range.m_end, range.m_step));
				},
				[&target_vm](const SerializedObject::FloatRange& range) -> MidoriTraceable*
				{
					return target_vm.AllocateTraceable(MidoriFloatRange(range.m_start, range.m_end, range.m_step));
				},
				[&target_vm](const SerializedObject::MutableCell& cell) -> MidoriTraceable*
				{
					return target_vm.AllocateCell(MidoriValue(), cell.m_holds_pointer);
				}
			},
			object.m_data
		);
	}
}

// Most messages are a number or a flag, which need none of the writer's tables.
TransferPayload ValueTransfer::Serialize(MidoriValue value, bool is_pointer, VirtualMachine& source_vm)
{
	if (!is_pointer)
	{
		return TransferPayload{ {}, {}, SerializedValue{ value.GetRawBits(), false } };
	}

	return PayloadWriter(source_vm).Write(value, true);
}

MidoriValue ValueTransfer::Deserialize(const TransferPayload& payload, VirtualMachine& target_vm)
{
	// Every object is allocated before any is filled in, so a reference to any of
	// them, back or forward, is a lookup.
	const std::vector<MidoriTraceable*> objects = payload.m_objects
		| std::views::transform([&target_vm](const SerializedObject& object) -> MidoriTraceable* { return Allocate(object, target_vm); })
		| std::ranges::to<std::vector<MidoriTraceable*>>();
	const auto resolve = [&objects](SerializedValue value) -> MidoriValue
	{
		return value.m_is_object ? MidoriValue(objects[static_cast<size_t>(value.m_bits)]) : MidoriValue::FromRawBits(value.m_bits);
	};

	for (size_t index = 0uz; index < objects.size(); index += 1uz)
	{
		MidoriTraceable* transferred = objects[index];
		std::visit
		(
			Overloaded
			{
				[transferred, &resolve](const SerializedObject::Array& array)
				{
					MidoriArray& elements = transferred->GetTraceable<MidoriArray>();
					for (size_t element = 0uz; element < array.m_elements.size(); element += 1uz)
					{
						elements[static_cast<int>(element)] = resolve(array.m_elements[element]);
					}
				},
				[transferred, &resolve](const SerializedObject::Aggregate& aggregate)
				{
					std::ranges::transform(aggregate.m_values, transferred->GetValues(), resolve);
				},
				[transferred, &resolve](const SerializedObject::MutableCell& cell)
				{
					transferred->GetTraceable<MidoriMutableCell>().m_value = resolve(cell.m_value);
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
