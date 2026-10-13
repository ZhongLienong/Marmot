#pragma once

#include "Value/Value.h"

#include <string>
#include <vector>
#include <variant>

class VirtualMachine;

// A value inside a TransferPayload: its own bits, or for an object, the index of
// the object's copy in the payload.
struct SerializedValue
{
	MidoriWord m_bits = 0u;
	bool m_is_object = false;
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
		bool m_holds_pointers = false;
	};

	// A tuple, struct, union or closure; `m_index` is a union's tag or a closure's
	// procedure, and a closure's values are what it captured.
	struct Aggregate
	{
		MidoriTraceable::TraceableType m_type = MidoriTraceable::TraceableType::Tuple;
		int m_index = 0;
		uint16_t m_shape = 0u;
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

	// A user Ref<T>, copied: two references to one cell in one payload arrive as
	// two references to one copy.
	struct MutableCell
	{
		SerializedValue m_value;
		bool m_holds_pointer = false;
	};

	using Variant = std::variant<Text, Array, Aggregate, IntRange, FloatRange, MutableCell>;

	Variant m_data;
};

struct SerializedGlobal
{
	int m_index = 0;
	SerializedValue m_value;
};

// What crosses from one VM to another: a value, every object it reaches, and the
// globals the functions among them can read. Objects refer to each other by
// index, so an object reached twice is one copy, and copying a structure of any
// depth in either direction takes no native stack.
struct TransferPayload
{
	std::vector<SerializedObject> m_objects;
	std::vector<SerializedGlobal> m_globals;
	SerializedValue m_root;
};

class ValueTransfer
{
public:
	// A value of the kind `is_pointer` says, which the compiler decided.
	static TransferPayload Serialize(MidoriValue value, bool is_pointer, VirtualMachine& source_vm);

	// The payload's value, rebuilt in `target_vm`'s heap. Its globals are offered
	// to `target_vm`, which keeps the ones it does not have yet.
	static MidoriValue Deserialize(const TransferPayload& payload, VirtualMachine& target_vm);
};
