#include "Support/Attributes/Attributes.h"
#include "Value.h"
#include "Error/RuntimeError.h"
#include "Interpreter/Allocator/MarmotAllocator.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <execution>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>

namespace
{
	// Size-classed recycler for MarmotArray, MarmotText and long aggregate
	// buffers. Those never migrate between threads (workers exchange values
	// only through serialization), so each thread recycles its own buffers.
	class ValueBufferPool
	{
	public:
		static constexpr size_t MIN_CLASS_SIZE = 32uz;
		static constexpr size_t MAX_CLASS_SIZE = 65536uz;

		~ValueBufferPool()
		{
			for (void* slab : m_slabs)
			{
				std::free(slab);
			}
		}

		MARMOT_NOINLINE void* Allocate(size_t& size)
		{
			const size_t class_index = GetClassIndex(size);
			size = MIN_CLASS_SIZE << class_index;

			FreeNode*& head = m_free_lists[class_index];
			if (head == nullptr && !Refill(class_index))
			{
				return nullptr;
			}

			FreeNode* node = m_free_lists[class_index];
			m_free_lists[class_index] = node->m_next;
			return node;
		}

		MARMOT_NOINLINE void Free(void* ptr, size_t size) noexcept
		{
			const size_t class_index = GetClassIndex(size);
			FreeNode* node = static_cast<FreeNode*>(ptr);
			node->m_next = m_free_lists[class_index];
			m_free_lists[class_index] = node;
		}

	private:
		struct FreeNode
		{
			FreeNode* m_next;
		};

		static constexpr size_t SLAB_SIZE = 65536uz;
		static constexpr size_t CLASS_COUNT = 12uz;

		static size_t GetClassIndex(size_t size) noexcept
		{
			const size_t rounded = size <= MIN_CLASS_SIZE ? MIN_CLASS_SIZE : std::bit_ceil(size);
			return static_cast<size_t>(std::bit_width(rounded) - std::bit_width(MIN_CLASS_SIZE));
		}

		MARMOT_NOINLINE bool Refill(size_t class_index)
		{
			void* slab = std::malloc(SLAB_SIZE);
			if (slab == nullptr)
			{
				return false;
			}
			m_slabs.push_back(slab);

			const size_t class_size = MIN_CLASS_SIZE << class_index;
			uint8_t* chunk = static_cast<uint8_t*>(slab);
			for (size_t i = 0uz; i < SLAB_SIZE / class_size; i += 1uz)
			{
				FreeNode* node = reinterpret_cast<FreeNode*>(chunk);
				node->m_next = m_free_lists[class_index];
				m_free_lists[class_index] = node;
				chunk += class_size;
			}
			return true;
		}

		std::array<FreeNode*, CLASS_COUNT> m_free_lists{};
		std::vector<void*> m_slabs;
	};

	ValueBufferPool& GetValueBufferPool()
	{
		thread_local ValueBufferPool s_pool;
		return s_pool;
	}

	// Rounds the request up to the granted capacity; callers must record it and
	// pass the same value back to FreeValueBuffer.
	MARMOT_NOINLINE void* AllocateValueBuffer(size_t& size)
	{
		if (size > ValueBufferPool::MAX_CLASS_SIZE)
		{
			return std::malloc(size);
		}
		return GetValueBufferPool().Allocate(size);
	}

	MARMOT_NOINLINE void FreeValueBuffer(void* ptr, size_t size) noexcept
	{
		if (ptr == nullptr)
		{
			return;
		}

		if (size > ValueBufferPool::MAX_CLASS_SIZE)
		{
			std::free(ptr);
			return;
		}
		GetValueBufferPool().Free(ptr, size);
	}
}

namespace UTF8
{
	bool IsContinuationByte(char byte)
	{
		return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
	}

	int GetCharacterByteCount(const char* str)
	{
		unsigned char first_byte = static_cast<unsigned char>(*str);

		if ((first_byte & 0x80u) == 0x00u)
		{
			return 1; // ASCII: 0xxxxxxx
		}
		else if ((first_byte & 0xE0u) == 0xC0u)
		{
			return 2; // 110xxxxx
		}
		else if ((first_byte & 0xF0u) == 0xE0u)
		{
			return 3; // 1110xxxx
		}
		else if ((first_byte & 0xF8u) == 0xF0u)
		{
			return 4; // 11110xxx
		}
		else
		{
			return 1; // Invalid UTF-8, treat as single byte
		}
	}

	void Append(std::string& encoded, std::uint32_t code_point)
	{
		if (code_point < 0x80u)
		{
			encoded.push_back(static_cast<char>(code_point));
		}
		else if (code_point < 0x800u)
		{
			encoded.push_back(static_cast<char>(0xC0u | (code_point >> 6u)));
			encoded.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
		}
		else if (code_point < 0x10000u)
		{
			encoded.push_back(static_cast<char>(0xE0u | (code_point >> 12u)));
			encoded.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
			encoded.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
		}
		else
		{
			encoded.push_back(static_cast<char>(0xF0u | (code_point >> 18u)));
			encoded.push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu)));
			encoded.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
			encoded.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
		}
	}

	int CountCodePoints(const char* str, int byte_length)
	{
		int count = 0;
		for (int i = 0; i < byte_length;)
		{
			int char_bytes = GetCharacterByteCount(str + i);
			count += 1;
			i += char_bytes;
		}
		return count;
	}

	int GetByteOffsetOfCodePoint(const char* str, int byte_length, int code_point_index)
	{
		int count = 0;
		int offset = 0;
		while (offset < byte_length && count < code_point_index)
		{
			int char_bytes = GetCharacterByteCount(str + offset);
			offset += char_bytes;
			count += 1;
		}
		return offset;
	}

	int StepBackward(const char* str, int current_offset)
	{
		if (current_offset <= 0)
		{
			return 0;
		}

		int offset = current_offset - 1;
		while (offset > 0 && IsContinuationByte(str[offset]))
		{
			offset -= 1;
		}
		return offset;
	}
}

MarmotText ConvertToQuotedText(const MarmotText& input)
{
	const int byte_len = input.GetByteLength();
	MarmotText result;
	result.Reserve(byte_len + 2);
	result.Append('\"');

	// Iterate over bytes to handle escape sequences (which are ASCII)
	const char* str = input.View().data();
	for (int i = 0; i < byte_len; i += 1)
	{
		char c = str[i];
		switch (c)
		{
			case '\n':
			{
				result.Append("\\n");
				break;
			}
			case '\t':
			{
				result.Append("\\t");
				break;
			}
			case '\r':
			{
				result.Append("\\r");
				break;
			}
			case '\\':
			{
				result.Append("\\\\");
				break;
			}
			case '\"':
			{
				result.Append("\\\"");
				break;
			}
			default:
			{
				// For non-escape characters, just append the byte
				// UTF-8 multi-byte sequences will be passed through correctly
				result.Append(c);
			}
		}
	}

	result.Append('\"');

	return result;
}

// Deliberately outside every MARMOT_DEBUG_* block: this exists to be compared
// across build configurations, so it must be compiled in all of them.
std::size_t MarmotValue::LibrarySize() noexcept
{
	return sizeof(MarmotValue);
}

#if MARMOT_DEBUG_FULL
MarmotText MarmotValue::ToText() const
{
	switch (m_tag)
	{
		case MarmotValue::FLOAT:
			return MarmotText::FromFloat(GetFloat());
		case MarmotValue::INT:
			return MarmotText::FromInteger(GetInteger());
		case MarmotValue::BYTE:
			return MarmotText::FromInteger(static_cast<MarmotInteger>(GetByte()));
		case MarmotValue::WORD:
			return MarmotText::FromWord(GetWord());
		case MarmotValue::BOOL:
			return GetBool() ? "true" : "false";
		case MarmotValue::UNIT:
			return "()";
		case MarmotValue::POINTER:
			return GetPointer()->ToText();
		default:
			return "!!!UNKNOWN!!!";
	}
}

bool MarmotValue::IsPointer() const noexcept
{
	return m_tag == POINTER;
}

MarmotValue::DebugTypeTag MarmotValue::GetTag() const noexcept
{
	return m_tag;
}

bool MarmotValue::AgreesWithKind(bool is_pointer) const noexcept
{
	switch (m_tag)
	{
	case POINTER:
		return is_pointer;
	case UNKNOWN:
		return true;
	case UNIT:
		return !is_pointer || GetRawBits() == 0u;
	default:
		return !is_pointer;
	}
}
#endif

MarmotTraceable::MarmotTraceable(MarmotText&& str) noexcept : m_type(TraceableType::Text), m_holds_pointers(false), m_text(std::move(str))
{
}

MarmotTraceable::MarmotTraceable(MarmotArray&& array, bool holds_pointers) noexcept : m_type(TraceableType::Array), m_holds_pointers(holds_pointers), m_array(std::move(array))
{
}

MarmotTraceable::MarmotTraceable(MarmotIntRange&& range) noexcept : m_type(TraceableType::IntRange), m_holds_pointers(false), m_int_range(std::move(range))
{
}

MarmotTraceable::MarmotTraceable(MarmotFloatRange&& range) noexcept : m_type(TraceableType::FloatRange), m_holds_pointers(false), m_float_range(std::move(range))
{
}

MarmotTraceable::MarmotTraceable(MarmotMutableCell&& mutable_cell, bool holds_pointer) noexcept : m_type(TraceableType::MutableCell), m_holds_pointers(holds_pointer), m_mutable_cell(std::move(mutable_cell))
{
}

// Only the header and the values are written: the slot may be smaller than
// sizeof(MarmotTraceable).
MarmotTraceable::MarmotTraceable(TraceableType type, std::span<const MarmotValue> values, int index, uint16_t shape) noexcept
	: m_type(type), m_holds_pointers(false), m_index(static_cast<uint16_t>(index)), m_length(static_cast<uint16_t>(values.size())), m_shape(shape)
{
	if (values.size() > static_cast<size_t>(INLINE_CAPACITY))
	{
		InitializeExternalValues(values);
		return;
	}
	// Not memcpy: a variable-length one is a call into libc, and the closure or
	// union built here is usually read a few instructions later. It cost the
	// closure benchmark 18%.
	for (size_t i = 0uz; i < values.size(); i += 1uz)
	{
		m_inline_values[i] = values[i];
	}
}

void MarmotTraceable::InitializeExternalValues(std::span<const MarmotValue> values)
{
	size_t bytes = values.size_bytes();
	m_external_values = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
	if (m_external_values == nullptr)
	{
		FatalOutOfMemory("an aggregate's values", bytes);
	}
	std::memcpy(static_cast<void*>(m_external_values), values.data(), values.size_bytes());
}

MarmotTraceable::~MarmotTraceable()
{
	switch (m_type)
	{
	case TraceableType::Text:
		m_text.~MarmotText();
		break;
	case TraceableType::Array:
		m_array.~MarmotArray();
		break;
	case TraceableType::IntRange:
	case TraceableType::FloatRange:
	case TraceableType::MutableCell:
		break;
	case TraceableType::Tuple:
	case TraceableType::Struct:
	case TraceableType::Union:
	case TraceableType::Closure:
		if (m_length > static_cast<uint16_t>(INLINE_CAPACITY))
		{
			// The pool finds the size class from the requested bytes as well as
			// from the granted ones, so the length is enough to free it.
			FreeValueBuffer(m_external_values, static_cast<size_t>(m_length) * sizeof(MarmotValue));
		}
		break;
	}
}

#if MARMOT_DEBUG_FULL
MarmotText MarmotTraceable::ToText()
{
	switch (m_type)
	{
	case TraceableType::Text:
		return ConvertToQuotedText(m_text);
	case TraceableType::Array:
	{
		if (m_array.GetLength() == 0)
		{
			return MarmotText("[]");
		}

		const int len = m_array.GetLength();
		MarmotText result("[");
		result.Append(m_array[0].ToText());
		for (int idx = 1; idx < len; idx += 1)
		{
			result.Append(", ");
			result.Append(m_array[idx].ToText());
		}
		result.Append("]");
		return result;
	}
	case TraceableType::Tuple:
	{
		const MarmotValue* values = GetValues();
		if (GetLength() == 0)
		{
			return MarmotText("()");
		}

		MarmotText result("(");
		result.Append(values[0].ToText());
		for (int idx = 1; idx < GetLength(); idx += 1)
		{
			result.Append(", ");
			result.Append(values[idx].ToText());
		}
		result.Append(")");
		return result;
	}
	case TraceableType::IntRange:
		return MarmotText("IntRange");
	case TraceableType::FloatRange:
		return MarmotText("FloatRange");
	case TraceableType::MutableCell:
		return MarmotText("ref (").Append(m_mutable_cell.m_value.ToText()).Append(")");
	case TraceableType::Closure:
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "<closure at: %p>", (void*)this);
		return MarmotText(buffer);
	}
	case TraceableType::Union:
	case TraceableType::Struct:
	{
		const MarmotValue* values = GetValues();
		MarmotText result(m_type == TraceableType::Union ? "Union{" : "Struct{");
		for (int idx = 0; idx < GetLength(); idx += 1)
		{
			if (idx > 0)
			{
				result.Append(", ");
			}
			result.Append(values[idx].ToText());
		}
		result.Append("}");
		return result;
	}
	default:
		return MarmotText("Unknown MarmotTraceable");
	}
}
#endif

size_t MarmotTraceable::GetSize() const
{
	switch (m_type)
	{
	case TraceableType::Text:
		return MarmotAllocator::SlotSizeFor(HEADER_SIZE + sizeof(MarmotText)) + m_text.GetOwnedBytes();
	case TraceableType::Array:
		return MarmotAllocator::SlotSizeFor(HEADER_SIZE + sizeof(MarmotArray)) + m_array.GetCapacity();
	case TraceableType::IntRange:
		return MarmotAllocator::SlotSizeFor(HEADER_SIZE + sizeof(MarmotIntRange));
	case TraceableType::FloatRange:
		return MarmotAllocator::SlotSizeFor(HEADER_SIZE + sizeof(MarmotFloatRange));
	case TraceableType::MutableCell:
		return MarmotAllocator::SlotSizeFor(HEADER_SIZE + sizeof(MarmotMutableCell));
	case TraceableType::Tuple:
	case TraceableType::Struct:
	case TraceableType::Union:
	case TraceableType::Closure:
		break;
	}
	const size_t external_bytes = m_length > static_cast<uint16_t>(INLINE_CAPACITY) ? static_cast<size_t>(m_length) * sizeof(MarmotValue) : 0uz;
	return MarmotAllocator::SlotSizeFor(AggregateBytes(GetLength())) + external_bytes;
}

void* MarmotTraceable::operator new(size_t size) noexcept
{
	void* object = ::operator new(size);
	return object;
}

void* MarmotTraceable::operator new(size_t size, std::align_val_t al) noexcept
{
	void* object = ::operator new(size, al);
	return object;
}

void* MarmotTraceable::operator new(size_t, void* ptr) noexcept
{
	return ptr;
}

void* MarmotTraceable::operator new(size_t, std::align_val_t, void* ptr) noexcept
{
	return ptr;
}

void MarmotTraceable::operator delete(void*, void*) noexcept
{
}

void MarmotTraceable::operator delete(void*, std::align_val_t, void*) noexcept
{
}

void MarmotTraceable::operator delete(void* object, size_t size) noexcept
{
	MarmotTraceable* traceable = static_cast<MarmotTraceable*>(object);
	(void)traceable;  // Unused but needed for potential future debugging

	::operator delete(object, size);
}

void MarmotTraceable::operator delete(void* object, std::align_val_t al) noexcept
{
	::operator delete(object, al);
}

MarmotArray::MarmotArray()
{
	std::memset(this, 0, sizeof(MarmotArray));
	SetShortSize(0);
}

MarmotArray::MarmotArray(int size)
{
	if (size <= SOO_CAPACITY)
	{
		std::memset(this, 0, sizeof(MarmotArray));
		SetShortSize(size);
		for (int i = 0; i < size; i += 1)
		{
			new (&m_short.m_buffer[i]) MarmotValue();
		}
	}
	else
	{
		size_t bytes = static_cast<size_t>(size) * sizeof(MarmotValue);
		m_long.m_ptr = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MarmotArray::MarmotArray", bytes);
		}
		m_long.m_size = size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MarmotValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
		for (int i = 0; i < size; i += 1)
		{
			new (&m_long.m_ptr[i]) MarmotValue();
		}
	}
}

MarmotArray::MarmotArray(const MarmotArray& other)
{
	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MarmotArray));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_capacity) * sizeof(MarmotValue);
		m_long.m_ptr = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MarmotArray::MarmotArray copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MarmotValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MarmotValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
}

MarmotArray::MarmotArray(MarmotArray&& other) noexcept
{
	std::memcpy(this, &other, sizeof(MarmotArray));
	std::memset(&other, 0, sizeof(MarmotArray));
	other.SetShortSize(0);
}

MarmotArray& MarmotArray::operator=(const MarmotArray& other)
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MarmotValue));
	}

	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MarmotArray));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_capacity) * sizeof(MarmotValue);
		m_long.m_ptr = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MarmotArray::operator= copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MarmotValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MarmotValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
	return *this;
}

MarmotArray& MarmotArray::operator=(MarmotArray&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MarmotValue));
	}

	std::memcpy(this, &other, sizeof(MarmotArray));
	std::memset(&other, 0, sizeof(MarmotArray));
	other.SetShortSize(0);
	return *this;
}

MarmotArray::~MarmotArray()
{
	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MarmotValue));
	}
}

void MarmotArray::Expand(int new_capacity)
{
	if (IsShort())
	{
		int current_size = GetShortSize();
		int capacity = new_capacity > 0 ? new_capacity : (current_size < s_initial_capacity ? s_initial_capacity : current_size * 2);

		size_t bytes = static_cast<size_t>(capacity) * sizeof(MarmotValue);
		MarmotValue* new_data = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
		if (!new_data)
		{
			FatalOutOfMemory("MarmotArray::Expand", bytes);
		}

		std::memcpy(new_data, m_short.m_buffer, static_cast<size_t>(current_size) * sizeof(MarmotValue));

		m_long.m_ptr = new_data;
		m_long.m_size = current_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MarmotValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
	else
	{
		int capacity = new_capacity > 0 ? new_capacity : (m_long.m_capacity == 0 ? s_initial_capacity : m_long.m_capacity * 2);
		if (capacity <= m_long.m_capacity)
		{
			return;
		}

		const size_t old_bytes = static_cast<size_t>(m_long.m_capacity) * sizeof(MarmotValue);
		size_t new_bytes = static_cast<size_t>(capacity) * sizeof(MarmotValue);
		if (old_bytes > ValueBufferPool::MAX_CLASS_SIZE && new_bytes > ValueBufferPool::MAX_CLASS_SIZE)
		{
			MarmotValue* new_data = static_cast<MarmotValue*>(std::realloc(m_long.m_ptr, new_bytes));
			if (!new_data)
			{
				FatalOutOfMemory("MarmotArray::Expand", new_bytes);
			}
			m_long.m_ptr = new_data;
			m_long.m_capacity = capacity;
		}
		else
		{
			MarmotValue* new_data = static_cast<MarmotValue*>(AllocateValueBuffer(new_bytes));
			if (!new_data)
			{
				FatalOutOfMemory("MarmotArray::Expand", new_bytes);
			}
			std::memcpy(new_data, m_long.m_ptr, static_cast<size_t>(m_long.m_size) * sizeof(MarmotValue));
			FreeValueBuffer(m_long.m_ptr, old_bytes);
			m_long.m_ptr = new_data;
			m_long.m_capacity = static_cast<int>(new_bytes / sizeof(MarmotValue));
		}
	}
}

MarmotArray MarmotArray::Slice(MarmotInteger start, MarmotInteger end) const
{
	const MarmotInteger length = GetLength();
	const int first = static_cast<int>(std::clamp(start, MarmotInteger{ 0 }, length));
	const int last = static_cast<int>(std::clamp(end, MarmotInteger{ 0 }, length));
	if (last <= first)
	{
		return MarmotArray();
	}

	const int slice_len = last - first;
	const MarmotValue* source = IsShort() ? m_short.m_buffer : m_long.m_ptr;

	MarmotArray result;
	if (slice_len <= SOO_CAPACITY)
	{
		result.SetShortSize(slice_len);
		std::memcpy(result.m_short.m_buffer, source + first, static_cast<size_t>(slice_len) * sizeof(MarmotValue));
	}
	else
	{
		result.Expand(slice_len);
		result.m_long.m_size = slice_len;
		std::memcpy(result.m_long.m_ptr, source + first, static_cast<size_t>(slice_len) * sizeof(MarmotValue));
	}

	return result;
}

MarmotArray MarmotArray::Reverse() const
{
	const int len = GetLength();
	if (len <= 1)
	{
		return MarmotArray(*this);
	}

	MarmotArray result;
	if (len <= SOO_CAPACITY)
	{
		result.SetShortSize(len);
		for (int idx = 0; idx < len; idx += 1)
		{
			result.m_short.m_buffer[idx] = (*this)[len - 1 - idx];
		}
	}
	else
	{
		result.Expand(len);
		result.m_long.m_size = len;
		for (int idx = 0; idx < len; idx += 1)
		{
			result.m_long.m_ptr[idx] = (*this)[len - 1 - idx];
		}
	}

	return result;
}

void MarmotArray::AddBack(const MarmotValue& value)
{
	if (IsShort())
	{
		int len = GetShortSize();
		if (len < SOO_CAPACITY)
		{
			m_short.m_buffer[len] = value;
			SetShortSize(len + 1);
		}
		else
		{
			Expand(0);
			m_long.m_ptr[m_long.m_size] = value;
			m_long.m_size += 1;
		}
	}
	else
	{
		if (m_long.m_size >= m_long.m_capacity)
		{
			Expand(0);
		}
		m_long.m_ptr[m_long.m_size] = value;
		m_long.m_size += 1;
	}
}

void MarmotArray::Extend(const MarmotArray& other)
{
	int other_len = other.GetLength();
	if (other_len == 0)
	{
		return;
	}

	int current_len = GetLength();
	int new_len = current_len + other_len;

	if (IsShort())
	{
		if (new_len <= SOO_CAPACITY)
		{
			const MarmotValue* other_data = other.IsShort() ? other.m_short.m_buffer : other.m_long.m_ptr;
			std::memcpy(m_short.m_buffer + current_len, other_data, static_cast<size_t>(other_len) * sizeof(MarmotValue));
			SetShortSize(new_len);
			return;
		}

		int new_capacity = std::max(new_len, s_initial_capacity);
		Expand(new_capacity);
	}
	else if (new_len > m_long.m_capacity)
	{
		// The pool's power-of-two classes already amortize repeated growth, so
		// request the exact size to keep capacities (and the GC's byte
		// accounting) tight; above the pool ceiling keep geometric growth.
		int new_capacity = static_cast<size_t>(new_len) * sizeof(MarmotValue) > ValueBufferPool::MAX_CLASS_SIZE
			? std::max(new_len, m_long.m_capacity * 2)
			: new_len;
		Expand(new_capacity);
	}

	const MarmotValue* other_data = other.IsShort() ? other.m_short.m_buffer : other.m_long.m_ptr;
	std::memcpy(m_long.m_ptr + current_len, other_data, static_cast<size_t>(other_len) * sizeof(MarmotValue));
	m_long.m_size = new_len;
}

size_t MarmotArray::GetCapacity() const
{
	return IsShort() ? static_cast<size_t>(SOO_CAPACITY) * sizeof(MarmotValue) : static_cast<size_t>(m_long.m_capacity) * sizeof(MarmotValue);
}

MarmotArray MarmotArray::Concatenate(const MarmotArray& a, const MarmotArray& b)
{
	int a_len = a.GetLength();
	int b_len = b.GetLength();
	int total_len = a_len + b_len;

	MarmotArray result;
	if (total_len <= SOO_CAPACITY)
	{
		result.SetShortSize(total_len);
		if (a.IsShort())
		{
			std::memcpy(result.m_short.m_buffer, a.m_short.m_buffer, a_len * sizeof(MarmotValue));
		}
		else
		{
			std::memcpy(result.m_short.m_buffer, a.m_long.m_ptr, a_len * sizeof(MarmotValue));
		}

		if (b.IsShort())
		{
			std::memcpy(result.m_short.m_buffer + a_len, b.m_short.m_buffer, b_len * sizeof(MarmotValue));
		}
		else
		{
			std::memcpy(result.m_short.m_buffer + a_len, b.m_long.m_ptr, b_len * sizeof(MarmotValue));
		}
	}
	else
	{
		result.Expand(total_len); 
		result.m_long.m_size = total_len;

		MarmotValue* dest = result.m_long.m_ptr;
		if (a.IsShort())
			std::memcpy(dest, a.m_short.m_buffer, a_len * sizeof(MarmotValue));
		else
			std::memcpy(dest, a.m_long.m_ptr, a_len * sizeof(MarmotValue));

		if (b.IsShort())
			std::memcpy(dest + a_len, b.m_short.m_buffer, b_len * sizeof(MarmotValue));
		else
			std::memcpy(dest + a_len, b.m_long.m_ptr, b_len * sizeof(MarmotValue));
	}

	return result;
}

MarmotArray MarmotArray::FromFFI(MarmotValue* ffi_allocated_data, int length)
{
	MarmotArray result;
	if (ffi_allocated_data == nullptr || length <= 0)
	{
		std::free(ffi_allocated_data);
		return result;
	}

	if (length <= SOO_CAPACITY)
	{
		result.SetShortSize(length);
		std::memcpy(result.m_short.m_buffer, ffi_allocated_data, static_cast<size_t>(length) * sizeof(MarmotValue));
	}
	else
	{
		// Copy instead of adopting: the FFI buffer came from the CRT heap and
		// must not enter the pooled free lists.
		size_t bytes = static_cast<size_t>(length) * sizeof(MarmotValue);
		result.m_long.m_ptr = static_cast<MarmotValue*>(AllocateValueBuffer(bytes));
		if (!result.m_long.m_ptr)
		{
			FatalOutOfMemory("MarmotArray::FromFFI", bytes);
		}
		std::memcpy(result.m_long.m_ptr, ffi_allocated_data, static_cast<size_t>(length) * sizeof(MarmotValue));
		result.m_long.m_size = length;
		result.m_long.m_capacity = static_cast<int>(bytes / sizeof(MarmotValue));
		result.m_long.m_flag = 0;
		result.m_short.m_size_flag = 0;
	}

	std::free(ffi_allocated_data);
	return result;
}

MarmotIntRange::MarmotIntRange(MarmotInteger start, MarmotInteger end, MarmotInteger step)
	: m_start(start), m_end(end), m_step(step)
{
}

MarmotInteger MarmotIntRange::GetStart() const
{
	return m_start;
}

MarmotInteger MarmotIntRange::GetEnd() const
{
	return m_end;
}

MarmotInteger MarmotIntRange::GetStep() const
{
	return m_step;
}

MarmotFloatRange::MarmotFloatRange(MarmotFloat start, MarmotFloat end, MarmotFloat step)
	: m_start(start), m_end(end), m_step(step)
{
}

MarmotFloat MarmotFloatRange::GetStart() const
{
	return m_start;
}

MarmotFloat MarmotFloatRange::GetEnd() const
{
	return m_end;
}

MarmotFloat MarmotFloatRange::GetStep() const
{
	return m_step;
}

MarmotText::MarmotText()
{
	m_short.m_buffer[0] = '\0';
	SetShortSize(0);
}

MarmotText::MarmotText(const char* str)
	: MarmotText()
{
	if (str != nullptr)
	{
		AppendBytes(std::string_view(str), -1);
	}
}

MarmotText::MarmotText(const MarmotText& other)
{
	std::memcpy(this, &other, sizeof(MarmotText));
	if (!IsShort())
	{
		GetBuffer().m_references += 1;
		m_long.m_owned_bytes = 0;
	}
}

MarmotText::MarmotText(MarmotText&& other) noexcept
{
	std::memcpy(this, &other, sizeof(MarmotText));
	other.m_short.m_buffer[0] = '\0';
	other.SetShortSize(0);
}

MarmotText& MarmotText::operator=(const MarmotText& other)
{
	if (this == &other)
	{
		return *this;
	}

	Release();
	std::memcpy(this, &other, sizeof(MarmotText));
	if (!IsShort())
	{
		GetBuffer().m_references += 1;
		m_long.m_owned_bytes = 0;
	}
	return *this;
}

MarmotText& MarmotText::operator=(MarmotText&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	Release();
	std::memcpy(this, &other, sizeof(MarmotText));
	other.m_short.m_buffer[0] = '\0';
	other.SetShortSize(0);
	return *this;
}

MarmotText::~MarmotText()
{
	Release();
}

int MarmotText::GetLength() const noexcept
{
	if (IsShort())
	{
		return UTF8::CountCodePoints(m_short.m_buffer, GetShortSize());
	}
	if (m_long.m_length_cache == -1)
	{
		m_long.m_length_cache = UTF8::CountCodePoints(m_long.m_ptr, m_long.m_size);
	}
	return m_long.m_length_cache;
}

MarmotText& MarmotText::Pop()
{
	const int size = GetByteLength();
	if (size == 0)
	{
		return *this;
	}
	if (IsShort())
	{
		const int new_size = UTF8::StepBackward(m_short.m_buffer, size);
		m_short.m_buffer[new_size] = '\0';
		SetShortSize(new_size);
		return *this;
	}

	if (!IsPrivate())
	{
		Reallocate(size);
	}
	const int new_size = UTF8::StepBackward(m_long.m_ptr, size);
	m_long.m_ptr[new_size] = '\0';
	m_long.m_size = new_size;
	GetBuffer().m_written = new_size;
	if (m_long.m_length_cache > 0)
	{
		m_long.m_length_cache -= 1;
	}
	return *this;
}

MarmotText& MarmotText::Append(const char* str)
{
	return str == nullptr ? *this : AppendBytes(std::string_view(str), -1);
}

MarmotText& MarmotText::Append(char c)
{
	return AppendBytes(std::string_view(&c, 1uz), UTF8::IsContinuationByte(c) ? 0 : 1);
}

MarmotText& MarmotText::Append(const MarmotText& other)
{
	if (&other == this)
	{
		const MarmotText copy(other);
		return Append(copy);
	}
	return AppendBytes(other.View(), other.IsShort() ? -1 : other.m_long.m_length_cache);
}

void MarmotText::Reserve(int capacity)
{
	if (IsShort() ? capacity > SSO_CAPACITY : !CanWriteAtEnd(capacity))
	{
		Reallocate(capacity);
	}
}

MarmotText& MarmotText::Prepend(const char* str)
{
	return str == nullptr ? *this : PrependBytes(std::string_view(str));
}

MarmotText& MarmotText::Prepend(char c)
{
	return PrependBytes(std::string_view(&c, 1uz));
}

MarmotText& MarmotText::Prepend(const MarmotText& other)
{
	if (&other == this)
	{
		const MarmotText copy(other);
		return Prepend(copy);
	}
	return PrependBytes(other.View());
}

MarmotText MarmotText::Substring(int start, int end) const
{
	const int len = GetLength();
	start = std::clamp(start, 0, len);
	end = std::clamp(end, 0, len);
	if (end <= start)
	{
		return MarmotText();
	}

	// The length is cached, and a text as long in code points as in bytes is
	// ASCII, where an index is its own byte offset. Walking to the offset made
	// reading a text one code point at a time quadratic.
	const char* source = View().data();
	const int byte_len = GetByteLength();
	const bool is_ascii = len == byte_len;
	const int start_offset = is_ascii ? start : UTF8::GetByteOffsetOfCodePoint(source, byte_len, start);
	const int end_offset = is_ascii ? end : UTF8::GetByteOffsetOfCodePoint(source, byte_len, end);
	std::string slice(source + start_offset, source + end_offset);
	return MarmotText(slice.c_str());
}

std::vector<MarmotText> MarmotText::Split(const MarmotText& delimiter) const
{
	std::vector<MarmotText> parts;
	const std::string_view source = View();
	const std::string_view needle = delimiter.View();

	if (needle.empty())
	{
		if (source.empty())
		{
			parts.emplace_back("");
			return parts;
		}

		for (size_t offset = 0u; offset < source.size();)
		{
			const int char_bytes = UTF8::GetCharacterByteCount(source.data() + offset);
			parts.emplace_back(std::string(source.substr(offset, static_cast<size_t>(char_bytes))).c_str());
			offset += static_cast<size_t>(char_bytes);
		}
		return parts;
	}

	size_t start = 0u;
	while (true)
	{
		const size_t found = source.find(needle, start);
		if (found == std::string_view::npos)
		{
			parts.emplace_back(std::string(source.substr(start)).c_str());
			return parts;
		}

		parts.emplace_back(std::string(source.substr(start, found - start)).c_str());
		start = found + needle.size();
	}
}

MarmotText MarmotText::Reverse() const
{
	const char* source = View().data();
	const int byte_len = GetByteLength();
	std::string reversed;
	reversed.reserve(static_cast<size_t>(byte_len));

	for (int end = byte_len; end > 0;)
	{
		const int start = UTF8::StepBackward(source, end);
		reversed.append(source + start, static_cast<size_t>(end - start));
		end = start;
	}

	return MarmotText(reversed.c_str());
}

std::vector<MarmotInteger> MarmotText::CodePoints() const
{
	const std::string_view bytes = View();
	std::vector<MarmotInteger> code_points;
	for (size_t offset = 0u; offset < bytes.size();)
	{
		const size_t length = static_cast<size_t>(UTF8::GetCharacterByteCount(bytes.data() + offset));
		const unsigned char lead = static_cast<unsigned char>(bytes[offset]);
		std::uint32_t code_point = (length == 1u) ? lead
			: (length == 2u) ? (lead & 0x1Fu)
			: (length == 3u) ? (lead & 0x0Fu)
			: (lead & 0x07u);
		for (size_t continuation = 1u; (continuation < length) && (offset + continuation < bytes.size()); continuation += 1u)
		{
			code_point = (code_point << 6u) | (static_cast<unsigned char>(bytes[offset + continuation]) & 0x3Fu);
		}
		code_points.push_back(static_cast<MarmotInteger>(code_point));
		offset += length;
	}
	return code_points;
}

MarmotText MarmotText::FromCodePoints(std::span<const MarmotInteger> code_points)
{
	std::string encoded;
	encoded.reserve(code_points.size());
	for (const MarmotInteger code_point : code_points)
	{
		UTF8::Append(encoded, static_cast<std::uint32_t>(code_point));
	}
	return MarmotText(encoded.c_str());
}

bool MarmotText::Contains(const MarmotText& other) const
{
	return View().find(other.View()) != std::string_view::npos;
}

MarmotText MarmotText::Replace(const MarmotText& old_value, const MarmotText& new_value) const
{
	const std::string_view source = View();
	const std::string_view needle = old_value.View();
	const std::string_view replacement = new_value.View();

	if (needle.empty())
	{
		return MarmotText(*this);
	}

	std::string replaced;
	size_t start = 0u;
	while (true)
	{
		const size_t found = source.find(needle, start);
		if (found == std::string_view::npos)
		{
			replaced.append(source.substr(start));
			break;
		}

		replaced.append(source.substr(start, found - start));
		replaced.append(replacement);
		start = found + needle.size();
	}

	return MarmotText(replaced.c_str());
}

MarmotText MarmotText::Trim() const
{
	const std::string_view source = View();
	size_t start = 0u;
	while (start < source.size() && std::isspace(static_cast<unsigned char>(source[start])) != 0)
	{
		start += 1u;
	}

	size_t end = source.size();
	while (end > start && std::isspace(static_cast<unsigned char>(source[end - 1u])) != 0)
	{
		end -= 1u;
	}

	return MarmotText(std::string(source.substr(start, end - start)).c_str());
}

char MarmotText::operator[](int index) const
{
	const std::string_view bytes = View();
	return bytes[static_cast<size_t>(UTF8::GetByteOffsetOfCodePoint(bytes.data(), GetByteLength(), index))];
}

bool MarmotText::operator==(const MarmotText& other) const
{
	return View() == other.View();
}

bool MarmotText::operator!=(const MarmotText& other) const
{
	return !(*this == other);
}

std::optional<MarmotInteger> MarmotText::ParseInteger() const
{
	const std::string_view bytes = View();
	const char* begin = bytes.data();
	const char* end = begin + bytes.size();
	MarmotInteger value = 0;
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if ((begin == end) || (result.ec != std::errc()) || (result.ptr != end))
	{
		return std::nullopt;
	}
	return value;
}

std::optional<MarmotFloat> MarmotText::ParseFloat() const
{
	const std::string_view bytes = View();
	const char* begin = bytes.data();
	const char* end = begin + bytes.size();
	MarmotFloat value = 0.0;
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if ((begin == end) || (result.ec != std::errc()) || (result.ptr != end))
	{
		return std::nullopt;
	}
	return value;
}

MarmotText MarmotText::FromInteger(MarmotInteger value)
{
	char buffer[32];
	std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer), value);
	if (result.ec != std::errc())
	{
		return MarmotText();
	}
	*result.ptr = '\0';
	return MarmotText(buffer);
}

MarmotText MarmotText::FromWord(MarmotWord value)
{
	char buffer[32];
	std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer), value);
	if (result.ec != std::errc())
	{
		return MarmotText();
	}
	*result.ptr = '\0';
	return MarmotText(buffer);
}

// The shortest text that reads back as the same Float, and one that still
// looks like a Float when the value is whole: 3.0, not 3.
MarmotText MarmotText::FromFloat(MarmotFloat value)
{
	// A NaN's sign bit means nothing, and 0.0 / 0.0 sets it on x86 but not on
	// ARM, so printing it made the same program's output differ by machine.
	if (std::isnan(value))
	{
		return MarmotText("nan");
	}

	char buffer[64];
	const std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer) - 3, value);
	std::string_view written(std::begin(buffer), result.ptr);
	char* end = result.ptr;
	if (written.find_first_of(".eEni") == std::string_view::npos)
	{
		*end++ = '.';
		*end++ = '0';
	}
	*end = '\0';
	return MarmotText(buffer);
}

// `a` copied shares its buffer, so appending `b` to the copy writes in place
// when `a` ends where the buffer has been written to, and otherwise moves the
// copy to a buffer twice `a`'s size: a chain of `++` is linear either way.
MarmotText MarmotText::Concatenate(const MarmotText& a, const MarmotText& b)
{
	MarmotText result(a);
	result.AppendBytes(b.View(), b.IsShort() ? -1 : b.m_long.m_length_cache);
	return result;
}

MarmotText MarmotText::FromFFI(char* ffi_allocated_string)
{
	// Copy instead of adopting: the FFI buffer came from the CRT heap and must
	// not enter the pooled free lists.
	if (!ffi_allocated_string)
	{
		return MarmotText();
	}

	MarmotText result(ffi_allocated_string);
	std::free(ffi_allocated_string);
	return result;
}

const char* MarmotText::CString()
{
	if (!IsShort() && m_long.m_size != GetBuffer().m_written)
	{
		Reallocate(m_long.m_size);
	}
	return IsShort() ? m_short.m_buffer : m_long.m_ptr;
}

size_t MarmotText::GetOwnedBytes() const
{
	return IsShort() ? 0uz : static_cast<size_t>(m_long.m_owned_bytes);
}

MarmotText::Buffer& MarmotText::GetBuffer() const noexcept
{
	return *(reinterpret_cast<Buffer*>(m_long.m_ptr) - 1);
}

bool MarmotText::CanWriteAtEnd(int size) const noexcept
{
	return !IsShort() && m_long.m_size == GetBuffer().m_written && size <= GetBuffer().m_capacity;
}

bool MarmotText::IsPrivate() const noexcept
{
	return IsShort() || (GetBuffer().m_references == 1 && m_long.m_size == GetBuffer().m_written);
}

MARMOT_NOINLINE void MarmotText::Reallocate(int capacity)
{
	const std::string_view bytes = View();
	const int size = static_cast<int>(bytes.size());
	size_t granted = sizeof(Buffer) + static_cast<size_t>(std::max(capacity, size)) + 1uz;
	Buffer* buffer = static_cast<Buffer*>(AllocateValueBuffer(granted));
	if (buffer == nullptr)
	{
		FatalOutOfMemory("MarmotText::Reallocate", granted);
	}
	char* characters = reinterpret_cast<char*>(buffer + 1);
	std::memcpy(characters, bytes.data(), bytes.size());
	characters[size] = '\0';
	*buffer = Buffer{ 1, size, static_cast<int>(granted - sizeof(Buffer) - 1uz), 0 };

	const int length_cache = IsShort() ? -1 : m_long.m_length_cache;
	Release();
	m_long.m_ptr = characters;
	m_long.m_size = size;
	m_long.m_length_cache = length_cache;
	m_long.m_owned_bytes = static_cast<int>(granted);
	m_long.m_flag = 0;
}

void MarmotText::Release() noexcept
{
	if (IsShort())
	{
		return;
	}
	Buffer& buffer = GetBuffer();
	buffer.m_references -= 1;
	if (buffer.m_references == 0)
	{
		FreeValueBuffer(&buffer, sizeof(Buffer) + static_cast<size_t>(buffer.m_capacity) + 1uz);
	}
}

MarmotText& MarmotText::AppendBytes(std::string_view bytes, int length)
{
	if (bytes.empty())
	{
		return *this;
	}
	const int size = GetByteLength();
	const int new_size = size + static_cast<int>(bytes.size());
	if (IsShort() && new_size <= SSO_CAPACITY)
	{
		std::memcpy(m_short.m_buffer + size, bytes.data(), bytes.size());
		m_short.m_buffer[new_size] = '\0';
		SetShortSize(new_size);
		return *this;
	}

	if (!CanWriteAtEnd(new_size))
	{
		Reallocate(std::max(new_size, 2 * size));
	}
	std::memcpy(m_long.m_ptr + size, bytes.data(), bytes.size());
	m_long.m_ptr[new_size] = '\0';
	m_long.m_size = new_size;
	GetBuffer().m_written = new_size;
	m_long.m_length_cache = (m_long.m_length_cache == -1 || length == -1) ? -1 : m_long.m_length_cache + length;
	return *this;
}

MarmotText& MarmotText::PrependBytes(std::string_view bytes)
{
	if (bytes.empty())
	{
		return *this;
	}
	const int size = GetByteLength();
	const int new_size = size + static_cast<int>(bytes.size());
	if (IsShort() && new_size <= SSO_CAPACITY)
	{
		std::memmove(m_short.m_buffer + bytes.size(), m_short.m_buffer, static_cast<size_t>(size));
		std::memcpy(m_short.m_buffer, bytes.data(), bytes.size());
		m_short.m_buffer[new_size] = '\0';
		SetShortSize(new_size);
		return *this;
	}

	if (IsShort() || !IsPrivate() || new_size > GetBuffer().m_capacity)
	{
		Reallocate(std::max(new_size, 2 * size));
	}
	std::memmove(m_long.m_ptr + bytes.size(), m_long.m_ptr, static_cast<size_t>(size));
	std::memcpy(m_long.m_ptr, bytes.data(), bytes.size());
	m_long.m_ptr[new_size] = '\0';
	m_long.m_size = new_size;
	GetBuffer().m_written = new_size;
	m_long.m_length_cache = -1;
	return *this;
}

MarmotMutableCell::MarmotMutableCell(MarmotValue value) noexcept
	: m_value(value)
{
}

