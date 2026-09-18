#pragma once

#include "Common/Value/Value.h"

#include <expected>
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

	struct Tuple
	{
		std::vector<SerializedValue> m_elements;
	};

	struct Struct
	{
		std::vector<SerializedValue> m_fields;
	};

	struct Union
	{
		int m_tag = 0;
		std::vector<SerializedValue> m_fields;
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

	// A function value: its procedure plus the cells it captured. Only the
	// runtime transfers these (the globals a worker starts from); user-visible
	// transfers are gated by `Transferable`, which closures do not satisfy.
	struct Closure
	{
		int m_proc_index = 0;
		std::vector<SerializedValue> m_cells;
	};

	struct Cell
	{
		SerializedValue m_value;
	};

	// A user Cell<T>. Copied into the worker; the sharing map keeps two
	// references to one cell in one payload pointing at one copy.
	struct MutableCell
	{
		SerializedValue m_value;
	};

	using Variant = std::variant<Text, Array, Tuple, Struct, Union, IntRange, FloatRange, Closure, Cell, MutableCell>;

	Variant m_data;
};

struct TransferResult
{
	MidoriValue m_value;
};

class ValueTransfer
{
public:
	// Maps from an object to its copy. Values serialised (or deserialised) through
	// the same map keep their sharing: two references to one object arrive as two
	// references to one copy. Everything that crosses in one spawn shares one map.
	using SerializePointerMap = std::unordered_map<MidoriTraceable*, std::shared_ptr<SerializedObject>>;
	using DeserializePointerMap = std::unordered_map<const SerializedObject*, MidoriTraceable*>;

	static std::expected<SerializedValue, std::string> Serialize(MidoriValue source, VirtualMachine& source_vm);

	static std::expected<SerializedValue, std::string> Serialize(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& sharing);

	static std::expected<MidoriValue, std::string> Deserialize(const SerializedValue& source, VirtualMachine& target_vm);

	static std::expected<MidoriValue, std::string> Deserialize(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& sharing);

	static std::expected<TransferResult, std::string> Transfer(MidoriValue source, VirtualMachine& source_vm, VirtualMachine& target_vm);

private:
	static std::expected<SerializedValue, std::string> SerializeValue(MidoriValue source, VirtualMachine& source_vm, SerializePointerMap& visited);

	static std::expected<std::shared_ptr<SerializedObject>, std::string> SerializeTraceable(MidoriTraceable* source, VirtualMachine& source_vm, SerializePointerMap& visited);

	static std::expected<MidoriValue, std::string> DeserializeValue(const SerializedValue& source, VirtualMachine& target_vm, DeserializePointerMap& visited);

	static std::expected<MidoriTraceable*, std::string> DeserializeObject(const std::shared_ptr<SerializedObject>& source, VirtualMachine& target_vm, DeserializePointerMap& visited);
};
