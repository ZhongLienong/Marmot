#include "Support/Attributes/Attributes.h"
#include "Value.h"
#include "Error/RuntimeError.h"

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
	[[noreturn]] void FatalOutOfMemory(const char* context, size_t bytes) noexcept
	{
		std::string message = "Out of memory while allocating " + std::to_string(bytes) + " bytes";
		if (context && *context != '\0')
		{
			message.append(" (").append(context).append(")");
		}
		message.push_back('.');

		const RuntimeError runtime_error(RuntimeErrorCode::MemoryAccessViolation, message);
		const std::string rendered(runtime_error.Rendered());
		std::fputs(rendered.c_str(), stderr);
		std::fputc('\n', stderr);
		std::fflush(stderr);
		std::exit(EXIT_FAILURE);
	}

	// Size-classed recycler for MidoriArray, MidoriTuple and MidoriText
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

		MIDORI_NOINLINE void* Allocate(size_t& size)
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

		MIDORI_NOINLINE void Free(void* ptr, size_t size) noexcept
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

		MIDORI_NOINLINE bool Refill(size_t class_index)
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
	MIDORI_NOINLINE void* AllocateValueBuffer(size_t& size)
	{
		if (size > ValueBufferPool::MAX_CLASS_SIZE)
		{
			return std::malloc(size);
		}
		return GetValueBufferPool().Allocate(size);
	}

	MIDORI_NOINLINE void FreeValueBuffer(void* ptr, size_t size) noexcept
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

MidoriText ConvertToQuotedText(const MidoriText& input)
{
	const int byte_len = input.GetByteLength();
	MidoriText result;
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

// Deliberately outside every MIDORI_DEBUG_* block: this exists to be compared
// across build configurations, so it must be compiled in all of them.
std::size_t MidoriValue::LibrarySize() noexcept
{
	return sizeof(MidoriValue);
}

#if MIDORI_DEBUG_FULL
MidoriText MidoriValue::ToText() const
{
	switch (m_tag)
	{
		case MidoriValue::FLOAT:
			return MidoriText::FromFloat(GetFloat());
		case MidoriValue::INT:
			return MidoriText::FromInteger(GetInteger());
		case MidoriValue::BYTE:
			return MidoriText::FromInteger(static_cast<MidoriInteger>(GetByte()));
		case MidoriValue::WORD:
			return MidoriText::FromWord(GetWord());
		case MidoriValue::BOOL:
			return GetBool() ? "true" : "false";
		case MidoriValue::UNIT:
			return "()";
		case MidoriValue::POINTER:
			return GetPointer()->ToText();
		default:
			return "!!!UNKNOWN!!!";
	}
}

bool MidoriValue::IsPointer() const noexcept
{
	return m_tag == POINTER;
}

MidoriValue::DebugTypeTag MidoriValue::GetTag() const noexcept
{
	return m_tag;
}
#endif

MidoriTraceable::MidoriTraceable(MidoriText&& str) noexcept : m_text(std::move(str)), m_type(TraceableType::Text)
{
}

MidoriTraceable::MidoriTraceable(MidoriArray&& array) noexcept : m_array(std::move(array)), m_type(TraceableType::Array)
{
}

MidoriTraceable::MidoriTraceable(MidoriTuple&& tuple) noexcept : m_tuple(std::move(tuple)), m_type(TraceableType::Tuple)
{
}

MidoriTraceable::MidoriTraceable(MidoriIntRange&& range) noexcept : m_int_range(std::move(range)), m_type(TraceableType::IntRange)
{
}

MidoriTraceable::MidoriTraceable(MidoriFloatRange&& range) noexcept : m_float_range(std::move(range)), m_type(TraceableType::FloatRange)
{
}

MidoriTraceable::MidoriTraceable(MidoriCellValue&& cell_value) noexcept : m_cell(std::move(cell_value)), m_type(TraceableType::Cell)
{
}

MidoriTraceable::MidoriTraceable(MidoriMutableCell&& mutable_cell) noexcept : m_mutable_cell(std::move(mutable_cell)), m_type(TraceableType::MutableCell)
{
}

MidoriTraceable::MidoriTraceable(MidoriClosure&& closure) noexcept : m_closure(std::move(closure)), m_type(TraceableType::Closure)
{
}

MidoriTraceable::MidoriTraceable(MidoriStruct&& midori_struct)noexcept : m_struct(std::move(midori_struct)), m_type(TraceableType::Struct)
{
}

MidoriTraceable::MidoriTraceable(MidoriUnion&& midori_union) noexcept : m_union(std::move(midori_union)), m_type(TraceableType::Union)
{
}

MidoriTraceable::MidoriTraceable(std::in_place_type_t<MidoriTuple>, std::span<const MidoriValue> values) noexcept : m_tuple(values), m_type(TraceableType::Tuple)
{
}

MidoriTraceable::MidoriTraceable(std::in_place_type_t<MidoriStruct>, std::span<const MidoriValue> values) noexcept : m_struct{ .m_values = MidoriTuple(values) }, m_type(TraceableType::Struct)
{
}

MidoriTraceable::MidoriTraceable(std::in_place_type_t<MidoriUnion>, std::span<const MidoriValue> values, int index) noexcept : m_union{ .m_values = MidoriTuple(values), .m_index = index }, m_type(TraceableType::Union)
{
}

MidoriTraceable::~MidoriTraceable()
{
	switch (m_type)
	{
	case TraceableType::Text:
		m_text.~MidoriText();
		break;
	case TraceableType::Array:
		m_array.~MidoriArray();
		break;
	case TraceableType::Tuple:
		m_tuple.~MidoriTuple();
		break;
	case TraceableType::IntRange:
		m_int_range.~MidoriIntRange();
		break;
	case TraceableType::FloatRange:
		m_float_range.~MidoriFloatRange();
		break;
	case TraceableType::Struct:
		m_struct.~MidoriStruct();
		break;
	case TraceableType::Union:
		m_union.~MidoriUnion();
		break;
	case TraceableType::Cell:
		m_cell.~MidoriCellValue();
		break;
	case TraceableType::MutableCell:
		m_mutable_cell.~MidoriMutableCell();
		break;
	case TraceableType::Closure:
		m_closure.~MidoriClosure();
		break;
	}
}

#if MIDORI_DEBUG_FULL
MidoriText MidoriTraceable::ToText()
{
	switch (m_type)
	{
	case TraceableType::Text:
		return ConvertToQuotedText(m_text);
	case TraceableType::Array:
	{
		if (m_array.GetLength() == 0)
		{
			return MidoriText("[]");
		}

		const int len = m_array.GetLength();
		MidoriText result("[");
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
		const int len = m_tuple.GetLength();
		if (len == 0)
		{
			return MidoriText("()");
		}

		MidoriText result("(");
		result.Append(m_tuple[0].ToText());
		for (int idx = 1; idx < len; idx += 1)
		{
			result.Append(", ");
			result.Append(m_tuple[idx].ToText());
		}
		result.Append(")");
		return result;
	}
	case TraceableType::IntRange:
		return MidoriText("IntRange");
	case TraceableType::FloatRange:
		return MidoriText("FloatRange");
	case TraceableType::Cell:
		return MidoriText("Cell(").Append(m_cell.GetValue().ToText()).Append(")");
	case TraceableType::MutableCell:
		return MidoriText("ref (").Append(m_mutable_cell.m_value.ToText()).Append(")");
	case TraceableType::Closure:
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "<closure at: %p>", (void*)this);
		return MidoriText(buffer);
	}
	case TraceableType::Union:
	{
		if (m_union.m_values.GetLength() == 0)
		{
			return MidoriText("Union{}");
		}

		const int len = m_union.m_values.GetLength();
		MidoriText union_val("Union{");
		union_val.Append(m_union.m_values[0].ToText());
		for (int idx = 1; idx < len; idx += 1)
		{
			union_val.Append(", ");
			union_val.Append(m_union.m_values[idx].ToText());
		}
		union_val.Append("}");
		return union_val;
	}
	case TraceableType::Struct:
	{
		if (m_struct.m_values.GetLength() == 0)
		{
			return MidoriText("Struct{}");
		}

		const int len = m_struct.m_values.GetLength();
		MidoriText struct_val("Struct{");
		struct_val.Append(m_struct.m_values[0].ToText());
		for (int idx = 1; idx < len; idx += 1)
		{
			struct_val.Append(", ");
			struct_val.Append(m_struct.m_values[idx].ToText());
		}
		struct_val.Append("}");
		return struct_val;
	}
	default:
		return MidoriText("Unknown MidoriTraceable");
	}
}
#endif

size_t MidoriTraceable::GetSize() const
{
	size_t dynamic_size = 0uz;
	switch (m_type)
	{
	case TraceableType::Text:
		dynamic_size = m_text.GetOwnedBytes();
		break;
	case TraceableType::Array:
		dynamic_size = m_array.GetCapacity();
		break;
	case TraceableType::Tuple:
		dynamic_size = m_tuple.GetCapacity();
		break;
	case TraceableType::Closure:
		dynamic_size = m_closure.m_cell_values.GetCapacity();
		break;
	case TraceableType::Struct:
		dynamic_size = m_struct.m_values.GetCapacity();
		break;
	case TraceableType::Union:
		dynamic_size = m_union.m_values.GetCapacity();
		break;
	default:
		break;
	}
	return sizeof(MidoriTraceable) + dynamic_size;
}

void* MidoriTraceable::operator new(size_t size) noexcept
{
	void* object = ::operator new(size);
	return object;
}

void* MidoriTraceable::operator new(size_t size, std::align_val_t al) noexcept
{
	void* object = ::operator new(size, al);
	return object;
}

void* MidoriTraceable::operator new(size_t, void* ptr) noexcept
{
	return ptr;
}

void* MidoriTraceable::operator new(size_t, std::align_val_t, void* ptr) noexcept
{
	return ptr;
}

void MidoriTraceable::operator delete(void*, void*) noexcept
{
}

void MidoriTraceable::operator delete(void*, std::align_val_t, void*) noexcept
{
}

void MidoriTraceable::operator delete(void* object, size_t size) noexcept
{
	MidoriTraceable* traceable = static_cast<MidoriTraceable*>(object);
	(void)traceable;  // Unused but needed for potential future debugging

	::operator delete(object, size);
}

void MidoriTraceable::operator delete(void* object, std::align_val_t al) noexcept
{
	::operator delete(object, al);
}

MidoriArray::MidoriArray()
{
	std::memset(this, 0, sizeof(MidoriArray));
	SetShortSize(0);
}

MidoriArray::MidoriArray(int size)
{
	if (size <= SOO_CAPACITY)
	{
		std::memset(this, 0, sizeof(MidoriArray));
		SetShortSize(size);
		for (int i = 0; i < size; i += 1)
		{
			new (&m_short.m_buffer[i]) MidoriValue();
		}
	}
	else
	{
		size_t bytes = static_cast<size_t>(size) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriArray::MidoriArray", bytes);
		}
		m_long.m_size = size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
		for (int i = 0; i < size; i += 1)
		{
			new (&m_long.m_ptr[i]) MidoriValue();
		}
	}
}

MidoriArray::MidoriArray(const MidoriArray& other)
{
	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MidoriArray));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_capacity) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriArray::MidoriArray copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
}

MidoriArray::MidoriArray(MidoriArray&& other) noexcept
{
	std::memcpy(this, &other, sizeof(MidoriArray));
	std::memset(&other, 0, sizeof(MidoriArray));
	other.SetShortSize(0);
}

MidoriArray& MidoriArray::operator=(const MidoriArray& other)
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}

	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MidoriArray));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_capacity) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriArray::operator= copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
	return *this;
}

MidoriArray& MidoriArray::operator=(MidoriArray&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}

	std::memcpy(this, &other, sizeof(MidoriArray));
	std::memset(&other, 0, sizeof(MidoriArray));
	other.SetShortSize(0);
	return *this;
}

MidoriArray::~MidoriArray()
{
	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}
}

void MidoriArray::Expand(int new_capacity)
{
	if (IsShort())
	{
		int current_size = GetShortSize();
		int capacity = new_capacity > 0 ? new_capacity : (current_size < s_initial_capacity ? s_initial_capacity : current_size * 2);

		size_t bytes = static_cast<size_t>(capacity) * sizeof(MidoriValue);
		MidoriValue* new_data = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!new_data)
		{
			FatalOutOfMemory("MidoriArray::Expand", bytes);
		}

		std::memcpy(new_data, m_short.m_buffer, static_cast<size_t>(current_size) * sizeof(MidoriValue));

		m_long.m_ptr = new_data;
		m_long.m_size = current_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
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

		const size_t old_bytes = static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue);
		size_t new_bytes = static_cast<size_t>(capacity) * sizeof(MidoriValue);
		if (old_bytes > ValueBufferPool::MAX_CLASS_SIZE && new_bytes > ValueBufferPool::MAX_CLASS_SIZE)
		{
			MidoriValue* new_data = static_cast<MidoriValue*>(std::realloc(m_long.m_ptr, new_bytes));
			if (!new_data)
			{
				FatalOutOfMemory("MidoriArray::Expand", new_bytes);
			}
			m_long.m_ptr = new_data;
			m_long.m_capacity = capacity;
		}
		else
		{
			MidoriValue* new_data = static_cast<MidoriValue*>(AllocateValueBuffer(new_bytes));
			if (!new_data)
			{
				FatalOutOfMemory("MidoriArray::Expand", new_bytes);
			}
			std::memcpy(new_data, m_long.m_ptr, static_cast<size_t>(m_long.m_size) * sizeof(MidoriValue));
			FreeValueBuffer(m_long.m_ptr, old_bytes);
			m_long.m_ptr = new_data;
			m_long.m_capacity = static_cast<int>(new_bytes / sizeof(MidoriValue));
		}
	}
}

MidoriArray MidoriArray::Slice(int start, int end) const
{
	const int len = GetLength();
	start = std::clamp(start, 0, len);
	end = std::clamp(end, 0, len);
	if (end <= start)
	{
		return MidoriArray();
	}

	const int slice_len = end - start;
	const MidoriValue* source = IsShort() ? m_short.m_buffer : m_long.m_ptr;

	MidoriArray result;
	if (slice_len <= SOO_CAPACITY)
	{
		result.SetShortSize(slice_len);
		std::memcpy(result.m_short.m_buffer, source + start, static_cast<size_t>(slice_len) * sizeof(MidoriValue));
	}
	else
	{
		result.Expand(slice_len);
		result.m_long.m_size = slice_len;
		std::memcpy(result.m_long.m_ptr, source + start, static_cast<size_t>(slice_len) * sizeof(MidoriValue));
	}

	return result;
}

MidoriArray MidoriArray::Reverse() const
{
	const int len = GetLength();
	if (len <= 1)
	{
		return MidoriArray(*this);
	}

	MidoriArray result;
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

bool MidoriArray::Contains(const MidoriValue& value) const
{
	// This is a raw-value comparison for now; typeclass-driven equality comes later.
	for (int idx = 0; idx < GetLength(); idx += 1)
	{
		if ((*this)[idx].GetRawBits() == value.GetRawBits())
		{
			return true;
		}
	}
	return false;
}

void MidoriArray::AddFront(const MidoriValue& value)
{
	int len = GetLength();
	
	bool needs_expand = false;
	if (IsShort())
	{
		if (len >= SOO_CAPACITY) needs_expand = true;
	}
	else
	{
		if (len >= m_long.m_capacity) needs_expand = true;
	}

	if (needs_expand)
	{
		Expand(0);
	}

	// Move elements
	if (IsShort())
	{
		std::memmove(m_short.m_buffer + 1, m_short.m_buffer, static_cast<size_t>(len) * sizeof(MidoriValue));
		m_short.m_buffer[0] = value;
		SetShortSize(len + 1);
	}
	else
	{
		std::memmove(m_long.m_ptr + 1, m_long.m_ptr, static_cast<size_t>(len) * sizeof(MidoriValue));
		m_long.m_ptr[0] = value;
		m_long.m_size += 1;
	}
}

void MidoriArray::AddBack(const MidoriValue& value)
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

void MidoriArray::Extend(const MidoriArray& other)
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
			const MidoriValue* other_data = other.IsShort() ? other.m_short.m_buffer : other.m_long.m_ptr;
			std::memcpy(m_short.m_buffer + current_len, other_data, static_cast<size_t>(other_len) * sizeof(MidoriValue));
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
		int new_capacity = static_cast<size_t>(new_len) * sizeof(MidoriValue) > ValueBufferPool::MAX_CLASS_SIZE
			? std::max(new_len, m_long.m_capacity * 2)
			: new_len;
		Expand(new_capacity);
	}

	const MidoriValue* other_data = other.IsShort() ? other.m_short.m_buffer : other.m_long.m_ptr;
	std::memcpy(m_long.m_ptr + current_len, other_data, static_cast<size_t>(other_len) * sizeof(MidoriValue));
	m_long.m_size = new_len;
}

size_t MidoriArray::GetCapacity() const
{
	return IsShort() ? static_cast<size_t>(SOO_CAPACITY) * sizeof(MidoriValue) : static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue);
}

MidoriArray MidoriArray::Concatenate(const MidoriArray& a, const MidoriArray& b)
{
	int a_len = a.GetLength();
	int b_len = b.GetLength();
	int total_len = a_len + b_len;

	MidoriArray result;
	if (total_len <= SOO_CAPACITY)
	{
		result.SetShortSize(total_len);
		if (a.IsShort())
		{
			std::memcpy(result.m_short.m_buffer, a.m_short.m_buffer, a_len * sizeof(MidoriValue));
		}
		else
		{
			std::memcpy(result.m_short.m_buffer, a.m_long.m_ptr, a_len * sizeof(MidoriValue));
		}

		if (b.IsShort())
		{
			std::memcpy(result.m_short.m_buffer + a_len, b.m_short.m_buffer, b_len * sizeof(MidoriValue));
		}
		else
		{
			std::memcpy(result.m_short.m_buffer + a_len, b.m_long.m_ptr, b_len * sizeof(MidoriValue));
		}
	}
	else
	{
		result.Expand(total_len); 
		result.m_long.m_size = total_len;

		MidoriValue* dest = result.m_long.m_ptr;
		if (a.IsShort())
			std::memcpy(dest, a.m_short.m_buffer, a_len * sizeof(MidoriValue));
		else
			std::memcpy(dest, a.m_long.m_ptr, a_len * sizeof(MidoriValue));

		if (b.IsShort())
			std::memcpy(dest + a_len, b.m_short.m_buffer, b_len * sizeof(MidoriValue));
		else
			std::memcpy(dest + a_len, b.m_long.m_ptr, b_len * sizeof(MidoriValue));
	}

	return result;
}

MidoriArray MidoriArray::FromFFI(MidoriValue* ffi_allocated_data, int length)
{
	MidoriArray result;
	if (ffi_allocated_data == nullptr || length <= 0)
	{
		std::free(ffi_allocated_data);
		return result;
	}

	if (length <= SOO_CAPACITY)
	{
		result.SetShortSize(length);
		std::memcpy(result.m_short.m_buffer, ffi_allocated_data, static_cast<size_t>(length) * sizeof(MidoriValue));
	}
	else
	{
		// Copy instead of adopting: the FFI buffer came from the CRT heap and
		// must not enter the pooled free lists.
		size_t bytes = static_cast<size_t>(length) * sizeof(MidoriValue);
		result.m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!result.m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriArray::FromFFI", bytes);
		}
		std::memcpy(result.m_long.m_ptr, ffi_allocated_data, static_cast<size_t>(length) * sizeof(MidoriValue));
		result.m_long.m_size = length;
		result.m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		result.m_long.m_flag = 0;
		result.m_short.m_size_flag = 0;
	}

	std::free(ffi_allocated_data);
	return result;
}

MidoriTuple::MidoriTuple()
{
	std::memset(this, 0, sizeof(MidoriTuple));
	SetShortSize(0);
}

MidoriTuple::MidoriTuple(int size)
{
	if (size <= SOO_CAPACITY)
	{
		std::memset(this, 0, sizeof(MidoriTuple));
		SetShortSize(size);
		for (int i = 0; i < size; i += 1)
		{
			new (&m_short.m_buffer[i]) MidoriValue();
		}
	}
	else
	{
		size_t bytes = static_cast<size_t>(size) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriTuple::MidoriTuple", bytes);
		}
		m_long.m_size = size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
}

// The long layout is out of line so the short one, which every constructor and
// union with up to SOO_CAPACITY fields takes, runs without a stack frame.
MidoriTuple::MidoriTuple(std::span<const MidoriValue> values)
{
	if (values.size() > static_cast<size_t>(SOO_CAPACITY))
	{
		InitializeLong(values);
		return;
	}

	std::memset(static_cast<void*>(this), 0, sizeof(MidoriTuple));
	SetShortSize(static_cast<int>(values.size()));
	for (size_t i = 0uz; i < values.size(); i += 1uz)
	{
		new (&m_short.m_buffer[i]) MidoriValue(values[i]);
	}
}

void MidoriTuple::InitializeLong(std::span<const MidoriValue> values)
{
	size_t bytes = values.size_bytes();
	m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
	if (!m_long.m_ptr)
	{
		FatalOutOfMemory("MidoriTuple::MidoriTuple", bytes);
	}
	std::memcpy(static_cast<void*>(m_long.m_ptr), values.data(), values.size_bytes());
	m_long.m_size = static_cast<int>(values.size());
	m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
	m_long.m_flag = 0;
	m_short.m_size_flag = 0;
}

MidoriTuple::MidoriTuple(const MidoriTuple& other)
{
	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MidoriTuple));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriTuple::MidoriTuple copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
}

MidoriTuple::MidoriTuple(MidoriTuple&& other) noexcept
{
	std::memcpy(this, &other, sizeof(MidoriTuple));
	std::memset(&other, 0, sizeof(MidoriTuple));
	other.SetShortSize(0);
}

MidoriTuple& MidoriTuple::operator=(const MidoriTuple& other)
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}

	if (other.IsShort())
	{
		std::memcpy(this, &other, sizeof(MidoriTuple));
	}
	else
	{
		size_t bytes = static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue);
		m_long.m_ptr = static_cast<MidoriValue*>(AllocateValueBuffer(bytes));
		if (!m_long.m_ptr)
		{
			FatalOutOfMemory("MidoriTuple::operator= copy", bytes);
		}
		std::memcpy(m_long.m_ptr, other.m_long.m_ptr, static_cast<size_t>(other.m_long.m_size) * sizeof(MidoriValue));
		m_long.m_size = other.m_long.m_size;
		m_long.m_capacity = static_cast<int>(bytes / sizeof(MidoriValue));
		m_long.m_flag = 0;
		m_short.m_size_flag = 0;
	}
	return *this;
}

MidoriTuple& MidoriTuple::operator=(MidoriTuple&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}

	std::memcpy(this, &other, sizeof(MidoriTuple));
	std::memset(&other, 0, sizeof(MidoriTuple));
	other.SetShortSize(0);
	return *this;
}

MidoriTuple::~MidoriTuple()
{
	if (!IsShort())
	{
		FreeValueBuffer(m_long.m_ptr, static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue));
	}
}

size_t MidoriTuple::GetCapacity() const
{
	if (IsShort())
	{
		return 0uz;
	}
	return static_cast<size_t>(m_long.m_capacity) * sizeof(MidoriValue);
}

MidoriIntRange::MidoriIntRange(MidoriInteger start, MidoriInteger end, MidoriInteger step)
	: m_start(start), m_end(end), m_step(step)
{
}

MidoriInteger MidoriIntRange::GetStart() const
{
	return m_start;
}

MidoriInteger MidoriIntRange::GetEnd() const
{
	return m_end;
}

MidoriInteger MidoriIntRange::GetStep() const
{
	return m_step;
}

MidoriFloatRange::MidoriFloatRange(MidoriFloat start, MidoriFloat end, MidoriFloat step)
	: m_start(start), m_end(end), m_step(step)
{
}

MidoriFloat MidoriFloatRange::GetStart() const
{
	return m_start;
}

MidoriFloat MidoriFloatRange::GetEnd() const
{
	return m_end;
}

MidoriFloat MidoriFloatRange::GetStep() const
{
	return m_step;
}

MidoriText::MidoriText()
{
	m_short.m_buffer[0] = '\0';
	SetShortSize(0);
}

MidoriText::MidoriText(const char* str)
	: MidoriText()
{
	if (str != nullptr)
	{
		AppendBytes(std::string_view(str), -1);
	}
}

MidoriText::MidoriText(const MidoriText& other)
{
	std::memcpy(this, &other, sizeof(MidoriText));
	if (!IsShort())
	{
		GetBuffer().m_references += 1;
		m_long.m_owned_bytes = 0;
	}
}

MidoriText::MidoriText(MidoriText&& other) noexcept
{
	std::memcpy(this, &other, sizeof(MidoriText));
	other.m_short.m_buffer[0] = '\0';
	other.SetShortSize(0);
}

MidoriText& MidoriText::operator=(const MidoriText& other)
{
	if (this == &other)
	{
		return *this;
	}

	Release();
	std::memcpy(this, &other, sizeof(MidoriText));
	if (!IsShort())
	{
		GetBuffer().m_references += 1;
		m_long.m_owned_bytes = 0;
	}
	return *this;
}

MidoriText& MidoriText::operator=(MidoriText&& other) noexcept
{
	if (this == &other)
	{
		return *this;
	}

	Release();
	std::memcpy(this, &other, sizeof(MidoriText));
	other.m_short.m_buffer[0] = '\0';
	other.SetShortSize(0);
	return *this;
}

MidoriText::~MidoriText()
{
	Release();
}

int MidoriText::GetLength() const noexcept
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

MidoriText& MidoriText::Pop()
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

MidoriText& MidoriText::Append(const char* str)
{
	return str == nullptr ? *this : AppendBytes(std::string_view(str), -1);
}

MidoriText& MidoriText::Append(char c)
{
	return AppendBytes(std::string_view(&c, 1uz), UTF8::IsContinuationByte(c) ? 0 : 1);
}

MidoriText& MidoriText::Append(const MidoriText& other)
{
	if (&other == this)
	{
		const MidoriText copy(other);
		return Append(copy);
	}
	return AppendBytes(other.View(), other.IsShort() ? -1 : other.m_long.m_length_cache);
}

void MidoriText::Reserve(int capacity)
{
	if (IsShort() ? capacity > SSO_CAPACITY : !CanWriteAtEnd(capacity))
	{
		Reallocate(capacity);
	}
}

MidoriText& MidoriText::Prepend(const char* str)
{
	return str == nullptr ? *this : PrependBytes(std::string_view(str));
}

MidoriText& MidoriText::Prepend(char c)
{
	return PrependBytes(std::string_view(&c, 1uz));
}

MidoriText& MidoriText::Prepend(const MidoriText& other)
{
	if (&other == this)
	{
		const MidoriText copy(other);
		return Prepend(copy);
	}
	return PrependBytes(other.View());
}

MidoriText MidoriText::Substring(int start, int end) const
{
	const int len = GetLength();
	start = std::clamp(start, 0, len);
	end = std::clamp(end, 0, len);
	if (end <= start)
	{
		return MidoriText();
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
	return MidoriText(slice.c_str());
}

std::vector<MidoriText> MidoriText::Split(const MidoriText& delimiter) const
{
	std::vector<MidoriText> parts;
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

MidoriText MidoriText::Reverse() const
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

	return MidoriText(reversed.c_str());
}

bool MidoriText::Contains(const MidoriText& other) const
{
	return View().find(other.View()) != std::string_view::npos;
}

MidoriText MidoriText::Replace(const MidoriText& old_value, const MidoriText& new_value) const
{
	const std::string_view source = View();
	const std::string_view needle = old_value.View();
	const std::string_view replacement = new_value.View();

	if (needle.empty())
	{
		return MidoriText(*this);
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

	return MidoriText(replaced.c_str());
}

MidoriText MidoriText::Trim() const
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

	return MidoriText(std::string(source.substr(start, end - start)).c_str());
}

char MidoriText::operator[](int index) const
{
	const std::string_view bytes = View();
	return bytes[static_cast<size_t>(UTF8::GetByteOffsetOfCodePoint(bytes.data(), GetByteLength(), index))];
}

bool MidoriText::operator==(const MidoriText& other) const
{
	return View() == other.View();
}

bool MidoriText::operator!=(const MidoriText& other) const
{
	return !(*this == other);
}

std::optional<MidoriInteger> MidoriText::ParseInteger() const
{
	const std::string_view bytes = View();
	const char* begin = bytes.data();
	const char* end = begin + bytes.size();
	MidoriInteger value = 0;
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if ((begin == end) || (result.ec != std::errc()) || (result.ptr != end))
	{
		return std::nullopt;
	}
	return value;
}

std::optional<MidoriFloat> MidoriText::ParseFloat() const
{
	const std::string_view bytes = View();
	const char* begin = bytes.data();
	const char* end = begin + bytes.size();
	MidoriFloat value = 0.0;
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if ((begin == end) || (result.ec != std::errc()) || (result.ptr != end))
	{
		return std::nullopt;
	}
	return value;
}

MidoriText MidoriText::FromInteger(MidoriInteger value)
{
	char buffer[32];
	std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer), value);
	if (result.ec != std::errc())
	{
		return MidoriText();
	}
	*result.ptr = '\0';
	return MidoriText(buffer);
}

MidoriText MidoriText::FromWord(MidoriWord value)
{
	char buffer[32];
	std::to_chars_result result = std::to_chars(std::begin(buffer), std::end(buffer), value);
	if (result.ec != std::errc())
	{
		return MidoriText();
	}
	*result.ptr = '\0';
	return MidoriText(buffer);
}

// The shortest text that reads back as the same Float, and one that still
// looks like a Float when the value is whole: 3.0, not 3.
MidoriText MidoriText::FromFloat(MidoriFloat value)
{
	// A NaN's sign bit means nothing, and 0.0 / 0.0 sets it on x86 but not on
	// ARM, so printing it made the same program's output differ by machine.
	if (std::isnan(value))
	{
		return MidoriText("nan");
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
	return MidoriText(buffer);
}

// `a` copied shares its buffer, so appending `b` to the copy writes in place
// when `a` ends where the buffer has been written to, and otherwise moves the
// copy to a buffer twice `a`'s size: a chain of `++` is linear either way.
MidoriText MidoriText::Concatenate(const MidoriText& a, const MidoriText& b)
{
	MidoriText result(a);
	result.AppendBytes(b.View(), b.IsShort() ? -1 : b.m_long.m_length_cache);
	return result;
}

MidoriText MidoriText::FromFFI(char* ffi_allocated_string)
{
	// Copy instead of adopting: the FFI buffer came from the CRT heap and must
	// not enter the pooled free lists.
	if (!ffi_allocated_string)
	{
		return MidoriText();
	}

	MidoriText result(ffi_allocated_string);
	std::free(ffi_allocated_string);
	return result;
}

const char* MidoriText::CString()
{
	if (!IsShort() && m_long.m_size != GetBuffer().m_written)
	{
		Reallocate(m_long.m_size);
	}
	return IsShort() ? m_short.m_buffer : m_long.m_ptr;
}

size_t MidoriText::GetOwnedBytes() const
{
	return IsShort() ? 0uz : static_cast<size_t>(m_long.m_owned_bytes);
}

MidoriText::Buffer& MidoriText::GetBuffer() const noexcept
{
	return *(reinterpret_cast<Buffer*>(m_long.m_ptr) - 1);
}

bool MidoriText::CanWriteAtEnd(int size) const noexcept
{
	return !IsShort() && m_long.m_size == GetBuffer().m_written && size <= GetBuffer().m_capacity;
}

bool MidoriText::IsPrivate() const noexcept
{
	return IsShort() || (GetBuffer().m_references == 1 && m_long.m_size == GetBuffer().m_written);
}

MIDORI_NOINLINE void MidoriText::Reallocate(int capacity)
{
	const std::string_view bytes = View();
	const int size = static_cast<int>(bytes.size());
	size_t granted = sizeof(Buffer) + static_cast<size_t>(std::max(capacity, size)) + 1uz;
	Buffer* buffer = static_cast<Buffer*>(AllocateValueBuffer(granted));
	if (buffer == nullptr)
	{
		FatalOutOfMemory("MidoriText::Reallocate", granted);
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

void MidoriText::Release() noexcept
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

MidoriText& MidoriText::AppendBytes(std::string_view bytes, int length)
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

MidoriText& MidoriText::PrependBytes(std::string_view bytes)
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

MidoriCellValue::MidoriCellValue() noexcept
	: m_value(MidoriValue())
{
}

MidoriCellValue::MidoriCellValue(MidoriValue value) noexcept
	: m_value(value)
{
}

MidoriValue& MidoriCellValue::GetValue()
{
	return m_value;
}

const MidoriValue& MidoriCellValue::GetValue() const
{
	return m_value;
}

MidoriMutableCell::MidoriMutableCell(MidoriValue value) noexcept
	: m_value(value)
{
}

