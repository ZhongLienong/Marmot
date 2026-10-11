#pragma once

#include "Value/Value.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <variant>

class VirtualMachine;

struct SerializedObject;

struct SerializedValue
{
	MidoriWord m_raw_bits = 0u;
	std::shared_ptr<SerializedObject> m_object;

	[[nodiscard]] bool HasObject() const noexcept
	{
		return m_object != nullptr;
	}
};

struct SerializedObject
{
	struct Text
	{
		std::string m_text;
	};

	struct Array
	{
		std::vector<SerializedValue> m_elements;
	};

	// A tuple, struct, union or closure; `m_index` is a union's tag or a closure's
	// procedure, and a closure's values are what it captured.
	struct Aggregate
	{
		MidoriTraceable::TraceableType m_type = MidoriTraceable::TraceableType::Tuple;
		int m_index = 0;
		std::vector<SerializedValue> m_values;
	};

	struct IntRange
	{
		MidoriInteger m_start = 0;
		MidoriInteger m_end = 0;
		MidoriInteger m_step = 0;
	};

	struct FloatRange
	{
		MidoriFloat m_start = 0.0;
		MidoriFloat m_end = 0.0;
		MidoriFloat m_step = 0.0;
	};

	// A user Ref<T>. Copied into the worker; the sharing map keeps two
	// references to one cell in one payload pointing at one copy.
	struct MutableCell
	{
		SerializedValue m_value;
	};

	using Variant = std::variant<Text, Array, Aggregate, IntRange, FloatRange, MutableCell>;

	Variant m_data;
};

class ValueTransfer
{
public:
	// Maps from an object to its copy. Values serialised (or deserialised) through
	// the same map keep their sharing: two references to one object arrive as two
	// references to one copy. Everything that crosses in one spawn shares one map.
	using SerializePointerMap = std::unordered_map<MidoriTraceable*, std::shared_ptr<SerializedObject>>;
	using DeserializePointerMap = std::unordered_map<const SerializedObject*, MidoriTraceable*>;

	static SerializedValue Serialize(MidoriValue source, VirtualMachine& source_vm);

	static SerializedValue Serialize(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& sharing);

	static MidoriValue Deserialize(const SerializedValue& source, VirtualMachine& target_vm);

	static MidoriValue Deserialize(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& sharing);

private:
	static std::shared_ptr<SerializedObject> SerializeTraceable(MidoriTraceable* source, VirtualMachine& source_vm, SerializePointerMap& visited);

	static MidoriTraceable* DeserializeObject(const SerializedObject& source, VirtualMachine& target_vm, DeserializePointerMap& visited);
};
