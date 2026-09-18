#include "ValueTransfer.h"

#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <format>

std::expected<SerializedValue, std::string> ValueTransfer::Serialize(MidoriValue source, VirtualMachine& source_vm)
{
	SerializePointerMap visited;
	return SerializeValue(source, source_vm, visited);
}

std::expected<SerializedValue, std::string> ValueTransfer::Serialize(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& sharing)
{
	return SerializeValue(source, source_vm, sharing);
}

std::expected<MidoriValue, std::string> ValueTransfer::Deserialize(const SerializedValue& source, VirtualMachine& target_vm)
{
	DeserializePointerMap visited;
	return DeserializeValue(source, target_vm, visited);
}

std::expected<MidoriValue, std::string> ValueTransfer::Deserialize(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& sharing)
{
	return DeserializeValue(source, target_vm, sharing);
}

std::expected<TransferResult, std::string> ValueTransfer::Transfer(MidoriValue source, VirtualMachine& source_vm, VirtualMachine& target_vm)
{
	std::expected<SerializedValue, std::string> serialized = Serialize(source, source_vm);
	if (!serialized.has_value())
	{
		return std::unexpected(serialized.error());
	}

	std::expected<MidoriValue, std::string> deserialized = Deserialize(serialized.value(), target_vm);
	if (!deserialized.has_value())
	{
		return std::unexpected(deserialized.error());
	}

	return TransferResult{ deserialized.value() };
}

std::expected<SerializedValue, std::string> ValueTransfer::SerializeValue(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& visited)
{
	MidoriTraceable* pointer = source.GetPointer();
	if (pointer == nullptr || !source_vm.GetGC().Contains(pointer))
	{
		return SerializedValue{ source.GetRawBits(), nullptr };
	}

	std::expected<std::shared_ptr<SerializedObject>, std::string> serialized_object = SerializeTraceable(pointer, source_vm, visited);
	if (!serialized_object.has_value())
	{
		return std::unexpected(serialized_object.error());
	}

	return SerializedValue{ 0u, serialized_object.value() };
}

std::expected<std::shared_ptr<SerializedObject>, std::string> ValueTransfer::SerializeTraceable(MidoriTraceable* source, VirtualMachine& source_vm, SerializePointerMap& visited)
{
	SerializePointerMap::iterator visited_it = visited.find(source);
	if (visited_it != visited.end())
	{
		return visited_it->second;
	}

	if (source->IsTraceable<MidoriText>())
	{
		const MidoriText& src_text = source->GetTraceable<MidoriText>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Text{ std::string(src_text.GetCString()) } });
		visited.emplace(source, serialized);
		return serialized;
	}

	if (source->IsTraceable<MidoriArray>())
	{
		const MidoriArray& src_array = source->GetTraceable<MidoriArray>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Array{} });
		visited.emplace(source, serialized);

		SerializedObject::Array& serialized_array = std::get<SerializedObject::Array>(serialized->m_data);
		const int length = src_array.GetLength();
		serialized_array.m_elements.reserve(static_cast<size_t>(length));
		for (int index = 0; index < length; index += 1)
		{
			std::expected<SerializedValue, std::string> element = SerializeValue(src_array[index], source_vm, visited);
			if (!element.has_value())
			{
				return std::unexpected(element.error());
			}
			serialized_array.m_elements.emplace_back(std::move(element.value()));
		}
		return serialized;
	}

	if (source->IsTraceable<MidoriTuple>())
	{
		const MidoriTuple& src_tuple = source->GetTraceable<MidoriTuple>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Tuple{} });
		visited.emplace(source, serialized);

		SerializedObject::Tuple& serialized_tuple = std::get<SerializedObject::Tuple>(serialized->m_data);
		const int length = src_tuple.GetLength();
		serialized_tuple.m_elements.reserve(static_cast<size_t>(length));
		for (int index = 0; index < length; index += 1)
		{
			std::expected<SerializedValue, std::string> element = SerializeValue(src_tuple[index], source_vm, visited);
			if (!element.has_value())
			{
				return std::unexpected(element.error());
			}
			serialized_tuple.m_elements.emplace_back(std::move(element.value()));
		}
		return serialized;
	}

	if (source->IsTraceable<MidoriStruct>())
	{
		const MidoriStruct& src_struct = source->GetTraceable<MidoriStruct>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Struct{} });
		visited.emplace(source, serialized);

		SerializedObject::Struct& serialized_struct = std::get<SerializedObject::Struct>(serialized->m_data);
		const int length = src_struct.m_values.GetLength();
		serialized_struct.m_fields.reserve(static_cast<size_t>(length));
		for (int index = 0; index < length; index += 1)
		{
			std::expected<SerializedValue, std::string> field = SerializeValue(src_struct.m_values[index], source_vm, visited);
			if (!field.has_value())
			{
				return std::unexpected(field.error());
			}
			serialized_struct.m_fields.emplace_back(std::move(field.value()));
		}
		return serialized;
	}

	if (source->IsTraceable<MidoriUnion>())
	{
		const MidoriUnion& src_union = source->GetTraceable<MidoriUnion>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Union{} });
		visited.emplace(source, serialized);

		SerializedObject::Union& serialized_union = std::get<SerializedObject::Union>(serialized->m_data);
		serialized_union.m_tag = src_union.m_index;
		const int length = src_union.m_values.GetLength();
		serialized_union.m_fields.reserve(static_cast<size_t>(length));
		for (int index = 0; index < length; index += 1)
		{
			std::expected<SerializedValue, std::string> field = SerializeValue(src_union.m_values[index], source_vm, visited);
			if (!field.has_value())
			{
				return std::unexpected(field.error());
			}
			serialized_union.m_fields.emplace_back(std::move(field.value()));
		}
		return serialized;
	}

	if (source->IsTraceable<MidoriIntRange>())
	{
		const MidoriIntRange& src_range = source->GetTraceable<MidoriIntRange>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>
		(
			SerializedObject
			{
				SerializedObject::IntRange
				{
					src_range.GetStart(),
					src_range.GetEnd(),
					src_range.GetStep()
				}
			}
		);
		visited.emplace(source, serialized);
		return serialized;
	}

	if (source->IsTraceable<MidoriFloatRange>())
	{
		const MidoriFloatRange& src_range = source->GetTraceable<MidoriFloatRange>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>
		(
			SerializedObject
			{
				SerializedObject::FloatRange
				{
					src_range.GetStart(),
					src_range.GetEnd(),
					src_range.GetStep()
				}
			}
		);
		visited.emplace(source, serialized);
		return serialized;
	}

	// Closures and cells are immutable once built (v2 has no assignment), so a
	// structural copy is the same value. A recursive local closure refers to itself
	// through its own cell; registering each object in `visited` before recursing
	// into its fields turns that cycle into a shared reference, as for arrays.
	if (source->IsTraceable<MidoriClosure>())
	{
		const MidoriClosure& src_closure = source->GetTraceable<MidoriClosure>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Closure{} });
		visited.emplace(source, serialized);

		SerializedObject::Closure& serialized_closure = std::get<SerializedObject::Closure>(serialized->m_data);
		serialized_closure.m_proc_index = src_closure.m_proc_index;
		const int length = src_closure.m_cell_values.GetLength();
		serialized_closure.m_cells.reserve(static_cast<size_t>(length));
		for (int index = 0; index < length; index += 1)
		{
			std::expected<SerializedValue, std::string> cell = SerializeValue(src_closure.m_cell_values[index], source_vm, visited);
			if (!cell.has_value())
			{
				return std::unexpected(cell.error());
			}
			serialized_closure.m_cells.emplace_back(std::move(cell.value()));
		}
		return serialized;
	}

	if (source->IsTraceable<MidoriCellValue>())
	{
		const MidoriCellValue& src_cell = source->GetTraceable<MidoriCellValue>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::Cell{} });
		visited.emplace(source, serialized);

		std::expected<SerializedValue, std::string> value = SerializeValue(src_cell.GetValue(), source_vm, visited);
		if (!value.has_value())
		{
			return std::unexpected(value.error());
		}
		std::get<SerializedObject::Cell>(serialized->m_data).m_value = std::move(value.value());
		return serialized;
	}

	if (source->IsTraceable<MidoriMutableCell>())
	{
		const MidoriMutableCell& src_cell = source->GetTraceable<MidoriMutableCell>();
		std::shared_ptr<SerializedObject> serialized = std::make_shared<SerializedObject>(SerializedObject{ SerializedObject::MutableCell{} });
		visited.emplace(source, serialized);

		std::expected<SerializedValue, std::string> value = SerializeValue(src_cell.m_value, source_vm, visited);
		if (!value.has_value())
		{
			return std::unexpected(value.error());
		}
		std::get<SerializedObject::MutableCell>(serialized->m_data).m_value = std::move(value.value());
		return serialized;
	}

	return std::unexpected(std::string("Cannot transfer unknown traceable value between workers."));
}

std::expected<MidoriValue, std::string> ValueTransfer::DeserializeValue(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& visited)
{
	if (!source.HasObject())
	{
		return MidoriValue::FromRawBits(source.m_raw_bits);
	}

	std::expected<MidoriTraceable*, std::string> deserialized_object = DeserializeObject(source.m_object, target_vm, visited);
	if (!deserialized_object.has_value())
	{
		return std::unexpected(deserialized_object.error());
	}

	return MidoriValue(deserialized_object.value());
}

std::expected<MidoriTraceable*, std::string> ValueTransfer::DeserializeObject(const std::shared_ptr<SerializedObject>& source, VirtualMachine& target_vm, DeserializePointerMap& visited)
{
	DeserializePointerMap::iterator visited_it = visited.find(source.get());
	if (visited_it != visited.end())
	{
		return visited_it->second;
	}

	if (const SerializedObject::Text* serialized_text = std::get_if<SerializedObject::Text>(&source->m_data))
	{
		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriText(serialized_text->m_text.c_str()));
		visited.emplace(source.get(), transferred);
		return transferred;
	}

	if (const SerializedObject::Array* serialized_array = std::get_if<SerializedObject::Array>(&source->m_data))
	{
		MidoriArray new_array(static_cast<int>(serialized_array->m_elements.size()));
		MidoriTraceable* transferred = target_vm.AllocateTraceable(std::move(new_array));
		visited.emplace(source.get(), transferred);

		MidoriArray& dst_array = transferred->GetTraceable<MidoriArray>();
		for (size_t index = 0u; index < serialized_array->m_elements.size(); index += 1u)
		{
			std::expected<MidoriValue, std::string> element = DeserializeValue(serialized_array->m_elements[index], target_vm, visited);
			if (!element.has_value())
			{
				return std::unexpected(element.error());
			}
			dst_array[static_cast<int>(index)] = element.value();
		}
		return transferred;
	}

	if (const SerializedObject::Tuple* serialized_tuple = std::get_if<SerializedObject::Tuple>(&source->m_data))
	{
		MidoriTuple new_tuple(static_cast<int>(serialized_tuple->m_elements.size()));
		MidoriTraceable* transferred = target_vm.AllocateTraceable(std::move(new_tuple));
		visited.emplace(source.get(), transferred);

		MidoriTuple& dst_tuple = transferred->GetTraceable<MidoriTuple>();
		for (size_t index = 0u; index < serialized_tuple->m_elements.size(); index += 1u)
		{
			std::expected<MidoriValue, std::string> element = DeserializeValue(serialized_tuple->m_elements[index], target_vm, visited);
			if (!element.has_value())
			{
				return std::unexpected(element.error());
			}
			dst_tuple[static_cast<int>(index)] = element.value();
		}
		return transferred;
	}

	if (const SerializedObject::Struct* serialized_struct = std::get_if<SerializedObject::Struct>(&source->m_data))
	{
		MidoriStruct new_struct;
		new_struct.m_values = MidoriTuple(static_cast<int>(serialized_struct->m_fields.size()));
		MidoriTraceable* transferred = target_vm.AllocateTraceable(std::move(new_struct));
		visited.emplace(source.get(), transferred);

		MidoriStruct& dst_struct = transferred->GetTraceable<MidoriStruct>();
		for (size_t index = 0u; index < serialized_struct->m_fields.size(); index += 1u)
		{
			std::expected<MidoriValue, std::string> field = DeserializeValue(serialized_struct->m_fields[index], target_vm, visited);
			if (!field.has_value())
			{
				return std::unexpected(field.error());
			}
			dst_struct.m_values[static_cast<int>(index)] = field.value();
		}
		return transferred;
	}

	if (const SerializedObject::Union* serialized_union = std::get_if<SerializedObject::Union>(&source->m_data))
	{
		MidoriUnion new_union;
		new_union.m_index = serialized_union->m_tag;
		new_union.m_values = MidoriTuple(static_cast<int>(serialized_union->m_fields.size()));
		MidoriTraceable* transferred = target_vm.AllocateTraceable(std::move(new_union));
		visited.emplace(source.get(), transferred);

		MidoriUnion& dst_union = transferred->GetTraceable<MidoriUnion>();
		for (size_t index = 0u; index < serialized_union->m_fields.size(); index += 1u)
		{
			std::expected<MidoriValue, std::string> field = DeserializeValue(serialized_union->m_fields[index], target_vm, visited);
			if (!field.has_value())
			{
				return std::unexpected(field.error());
			}
			dst_union.m_values[static_cast<int>(index)] = field.value();
		}
		return transferred;
	}

	if (const SerializedObject::IntRange* serialized_range = std::get_if<SerializedObject::IntRange>(&source->m_data))
	{
		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriIntRange(serialized_range->m_start, serialized_range->m_end, serialized_range->m_step));
		visited.emplace(source.get(), transferred);
		return transferred;
	}

	if (const SerializedObject::FloatRange* serialized_range = std::get_if<SerializedObject::FloatRange>(&source->m_data))
	{
		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriFloatRange(serialized_range->m_start, serialized_range->m_end, serialized_range->m_step));
		visited.emplace(source.get(), transferred);
		return transferred;
	}

	if (const SerializedObject::Closure* serialized_closure = std::get_if<SerializedObject::Closure>(&source->m_data))
	{
		// A capture-free function goes through the target VM's static closure
		// cache, exactly as MAKE_FUNCTION would produce it there.
		if (serialized_closure->m_cells.empty())
		{
			MidoriTraceable* function = target_vm.MakeFunctionValue(serialized_closure->m_proc_index).GetPointer();
			visited.emplace(source.get(), function);
			return function;
		}

		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriClosure{ .m_cell_values = MidoriTuple(static_cast<int>(serialized_closure->m_cells.size())), .m_proc_index = serialized_closure->m_proc_index });
		visited.emplace(source.get(), transferred);

		MidoriTuple& dst_cells = transferred->GetTraceable<MidoriClosure>().m_cell_values;
		for (size_t index = 0u; index < serialized_closure->m_cells.size(); index += 1u)
		{
			std::expected<MidoriValue, std::string> cell = DeserializeValue(serialized_closure->m_cells[index], target_vm, visited);
			if (!cell.has_value())
			{
				return std::unexpected(cell.error());
			}
			dst_cells[static_cast<int>(index)] = cell.value();
		}
		return transferred;
	}

	if (const SerializedObject::Cell* serialized_cell = std::get_if<SerializedObject::Cell>(&source->m_data))
	{
		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriCellValue());
		visited.emplace(source.get(), transferred);

		std::expected<MidoriValue, std::string> value = DeserializeValue(serialized_cell->m_value, target_vm, visited);
		if (!value.has_value())
		{
			return std::unexpected(value.error());
		}
		transferred->GetTraceable<MidoriCellValue>().GetValue() = value.value();
		return transferred;
	}

	if (const SerializedObject::MutableCell* serialized_cell = std::get_if<SerializedObject::MutableCell>(&source->m_data))
	{
		MidoriTraceable* transferred = target_vm.AllocateTraceable(MidoriMutableCell(MidoriValue()));
		visited.emplace(source.get(), transferred);

		std::expected<MidoriValue, std::string> value = DeserializeValue(serialized_cell->m_value, target_vm, visited);
		if (!value.has_value())
		{
			return std::unexpected(value.error());
		}
		transferred->GetTraceable<MidoriMutableCell>().m_value = value.value();
		return transferred;
	}

	return std::unexpected(std::string("Cannot deserialize unknown transferred value."));
}
