#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <unordered_set>
#include <variant>
#include <vector>

#include "Common/BuildConfig/BuildConfig.h"

class MidoriTraceable;
class MidoriText;

using MidoriInteger = int64_t;
using MidoriFloat = double;
using MidoriByte = uint8_t;
using MidoriWord = uint64_t;
using MidoriUnit = std::monostate;
using MidoriBool = bool;

class MidoriValue
{
public:
	constexpr static inline int DATA_BUFFER_SIZE = sizeof(double);

	// sizeof(MidoriValue) as compiled inside MidoriCore. Out of line on purpose,
	// so a consumer can compare it with its own sizeof(MidoriValue): the layout
	// below depends on MIDORI_DEBUG_FULL, and a mismatch means objects passed
	// across the library boundary are read with the wrong size.
	[[nodiscard]] static std::size_t LibrarySize() noexcept;
private:
	union
	{
		MidoriFloat m_float;
		MidoriInteger m_integer;
		MidoriBool m_bool;
		MidoriTraceable* m_pointer;
	} m_data;
#if MIDORI_DEBUG_FULL
	enum DebugTypeTag : int64_t
	{
		FLOAT = 0,
		INT,
		BYTE,
		WORD,
		BOOL,
		POINTER,
		UNIT,
		UNKNOWN,
	};

	DebugTypeTag m_tag;
#endif

public:
	// Defined inline: these are the hottest functions in the interpreter loop,
	// and relying on LTCG to inline them across translation units proved
	// fragile (unrelated growth in Value.cpp flipped the inlining decision).
	MIDORI_FORCE_INLINE MidoriValue() noexcept
		: m_data{.m_integer = 0}
#if MIDORI_DEBUG_FULL
		, m_tag(UNIT)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriFloat d) noexcept
		: m_data{.m_float = d}
#if MIDORI_DEBUG_FULL
		, m_tag(FLOAT)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriInteger l) noexcept
		: m_data{.m_integer = l}
#if MIDORI_DEBUG_FULL
		, m_tag(INT)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriByte byte) noexcept
		: m_data{.m_integer = static_cast<MidoriInteger>(byte)}
#if MIDORI_DEBUG_FULL
		, m_tag(BYTE)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriWord word) noexcept
		: m_data{.m_integer = static_cast<MidoriInteger>(word)}
#if MIDORI_DEBUG_FULL
		, m_tag(WORD)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriBool b) noexcept
		: m_data{.m_bool = b}
#if MIDORI_DEBUG_FULL
		, m_tag(BOOL)
#endif
	{
	}

	MIDORI_FORCE_INLINE MidoriValue(MidoriTraceable* o) noexcept
		: m_data{.m_pointer = o}
#if MIDORI_DEBUG_FULL
		, m_tag(POINTER)
#endif
	{
	}

	MidoriValue(const MidoriValue& other) noexcept = default;

	MidoriValue(MidoriValue&& other) noexcept = default;

	MidoriValue& operator=(const MidoriValue& other) noexcept = default;

	MidoriValue& operator=(MidoriValue&& other) noexcept = default;

	MIDORI_FORCE_INLINE MidoriFloat GetFloat() const noexcept
	{
		return m_data.m_float;
	}

	MIDORI_FORCE_INLINE MidoriInteger GetInteger() const noexcept
	{
		return m_data.m_integer;
	}

	MIDORI_FORCE_INLINE MidoriByte GetByte() const noexcept
	{
		return static_cast<MidoriByte>(m_data.m_integer & 0xFF);
	}

	MIDORI_FORCE_INLINE MidoriWord GetWord() const noexcept
	{
		return static_cast<MidoriWord>(m_data.m_integer);
	}

	MIDORI_FORCE_INLINE MidoriUnit GetUnit() const noexcept
	{
		return {};
	}

	MIDORI_FORCE_INLINE MidoriBool GetBool() const noexcept
	{
		return m_data.m_bool;
	}

	MIDORI_FORCE_INLINE MidoriTraceable* GetPointer() const noexcept
	{
		return m_data.m_pointer;
	}

	MIDORI_FORCE_INLINE const void* GetRawDataPtr() const noexcept
	{
		return &m_data;
	}

	MIDORI_FORCE_INLINE MidoriWord GetRawBits() const noexcept
	{
		static_assert(sizeof(MidoriWord) == sizeof(m_data));
		MidoriWord bits = 0u;
		std::memcpy(&bits, &m_data, sizeof(bits));
		return bits;
	}

	static MIDORI_FORCE_INLINE MidoriValue FromRawBits(MidoriWord bits) noexcept
	{
		MidoriValue value;
		static_assert(sizeof(MidoriWord) == sizeof(value.m_data));
		std::memcpy(&value.m_data, &bits, sizeof(bits));
#if MIDORI_DEBUG_FULL
		value.m_tag = UNKNOWN;
#endif
		return value;
	}

#if MIDORI_DEBUG_FULL
	MidoriText ToText() const;

	bool IsPointer() const noexcept;

	DebugTypeTag GetTag() const noexcept;
#endif
};

template<typename... Args>
concept MidoriValueConstructible = std::constructible_from<MidoriValue, Args...>;

template<typename T>
concept MidoriTraceableConstructible = std::constructible_from<MidoriTraceable, T>;

template <typename T>
concept MidoriNumeric = std::same_as<T, MidoriFloat> || std::same_as<T, MidoriInteger> || std::same_as<T, MidoriByte> || std::same_as<T, MidoriWord>;

class MidoriText
{
private:
	// Sized so MidoriTraceable fills one allocator slot; small payloads then
	// never need a separate heap buffer.
	static constexpr int STORAGE_SIZE = 72;
	static constexpr int SSO_CAPACITY = STORAGE_SIZE - 2;

	struct LongLayout
	{
		char* m_ptr;
		int m_size;
		int m_capacity;
		mutable int m_length_cache;
		uint8_t m_padding[STORAGE_SIZE - sizeof(char*) - 3uz * sizeof(int) - 1uz];
		uint8_t m_flag;
	};

	struct ShortLayout
	{
		char m_buffer[STORAGE_SIZE - 1];
		uint8_t m_size_flag;
	};

	static_assert(sizeof(LongLayout) == STORAGE_SIZE);
	static_assert(sizeof(ShortLayout) == STORAGE_SIZE);
	static_assert(offsetof(LongLayout, m_flag) == offsetof(ShortLayout, m_size_flag));

	union
	{
		LongLayout m_long;
		ShortLayout m_short;
	};

public:
	MidoriText();

	MidoriText(const char* str);

	MidoriText(const MidoriText& other);

	MidoriText(MidoriText&& other) noexcept;

	MidoriText& operator=(const MidoriText& other);

	MidoriText& operator=(MidoriText&& other) noexcept;

	~MidoriText();

	int GetLength() const noexcept;

	MIDORI_FORCE_INLINE int GetByteLength() const noexcept
	{
		return IsShort() ? GetShortSize() : m_long.m_size;
	}

	MIDORI_FORCE_INLINE const char* GetCString() const noexcept
	{
		return IsShort() ? m_short.m_buffer : m_long.m_ptr;
	}

	MidoriText& Pop();

	MidoriText& Append(const char* str);

	MidoriText& Append(char c);

	MidoriText& Append(const MidoriText& other);

	void Reserve(int capacity);

	MidoriText& Prepend(const char* str);

	MidoriText& Prepend(char c);

	MidoriText& Prepend(const MidoriText& other);

	MidoriText Substring(int start, int end) const;

	std::vector<MidoriText> Split(const MidoriText& delimiter) const;

	MidoriText Reverse() const;

	bool Contains(const MidoriText& other) const;

	MidoriText Replace(const MidoriText& old_value, const MidoriText& new_value) const;

	MidoriText Trim() const;

	char operator[](int index) const;

	bool operator==(const MidoriText& other) const;

	bool operator!=(const MidoriText& other) const;

	MidoriInteger ToInteger() const;

	MidoriFloat ToFloat() const;

	static MidoriText FromInteger(MidoriInteger value);

	static MidoriText FromWord(MidoriWord value);

	static MidoriText FromFloat(MidoriFloat value);

	static MidoriText Concatenate(const MidoriText& a, const MidoriText& b);

	static MidoriText FromFFI(char* ffi_allocated_string);

	size_t GetCapacity() const;

private:
	void Expand(int new_size);

	void GrowLongBuffer(int new_capacity);

	MIDORI_FORCE_INLINE bool IsShort() const noexcept
	{
		return (m_short.m_size_flag & 1) != 0;
	}

	MIDORI_FORCE_INLINE void SetShortSize(int size)
	{
		m_short.m_size_flag = static_cast<uint8_t>((size << 1) | 1);
	}

	MIDORI_FORCE_INLINE int GetShortSize() const
	{
		return m_short.m_size_flag >> 1;
	}
};

class MidoriArray
{
private:
	static constexpr int s_initial_capacity = 8;
	// Sized so MidoriTraceable fills one allocator slot; small payloads then
	// never need a separate heap buffer.
	static constexpr int STORAGE_SIZE = 72;
	static constexpr int SOO_CAPACITY = (STORAGE_SIZE - 8) / static_cast<int>(sizeof(MidoriValue));

	struct LongLayout
	{
		MidoriValue* m_ptr;
		int m_size;
		int m_capacity;
		uint8_t m_padding[STORAGE_SIZE - sizeof(MidoriValue*) - 2uz * sizeof(int) - 1uz];
		uint8_t m_flag;
	};

	struct ShortLayout
	{
		MidoriValue m_buffer[SOO_CAPACITY];
		uint8_t m_padding[STORAGE_SIZE - SOO_CAPACITY * sizeof(MidoriValue) - 1uz];
		uint8_t m_size_flag;
	};

	static_assert(sizeof(LongLayout) == STORAGE_SIZE);
	static_assert(sizeof(ShortLayout) == STORAGE_SIZE);
	static_assert(offsetof(LongLayout, m_flag) == offsetof(ShortLayout, m_size_flag));

	union
	{
		LongLayout m_long;
		ShortLayout m_short;
	};

public:
	MidoriArray();

	MidoriArray(int size);

	MidoriArray(const MidoriArray& other);

	MidoriArray(MidoriArray&& other) noexcept;

	MidoriArray& operator=(const MidoriArray& other);

	MidoriArray& operator=(MidoriArray&& other) noexcept;

	~MidoriArray();

	MIDORI_FORCE_INLINE MidoriValue& operator[](int index)
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	MIDORI_FORCE_INLINE const MidoriValue& operator[](int index) const
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	void AddBack(const MidoriValue& value);

	void AddFront(const MidoriValue& value);

	void Extend(const MidoriArray& other);

	MidoriArray Slice(int start, int end) const;

	MidoriArray Reverse() const;

	bool Contains(const MidoriValue& value) const;

	MIDORI_FORCE_INLINE int GetLength() const
	{
		return IsShort() ? GetShortSize() : m_long.m_size;
	}

	size_t GetCapacity() const;

	static MidoriArray Concatenate(const MidoriArray& a, const MidoriArray& b);

	static MidoriArray FromFFI(MidoriValue* ffi_allocated_data, int length);

private:
	void Expand(int new_capacity);

	MIDORI_FORCE_INLINE bool IsShort() const noexcept
	{
		return (m_short.m_size_flag & 1) != 0;
	}

	MIDORI_FORCE_INLINE void SetShortSize(int size)
	{
		m_short.m_size_flag = static_cast<uint8_t>((size << 1) | 1);
	}

	MIDORI_FORCE_INLINE int GetShortSize() const
	{
		return m_short.m_size_flag >> 1;
	}
};

class MidoriTuple
{
private:
	// Smaller than MidoriArray/MidoriText: MidoriClosure and MidoriUnion wrap a
	// tuple plus an int, and those must still fit the traceable slot budget.
	static constexpr int STORAGE_SIZE = 64;
	static constexpr int SOO_CAPACITY = (STORAGE_SIZE - 8) / static_cast<int>(sizeof(MidoriValue));

	struct LongLayout
	{
		MidoriValue* m_ptr;
		int m_size;
		int m_capacity;
		uint8_t m_padding[STORAGE_SIZE - sizeof(MidoriValue*) - 2uz * sizeof(int) - 1uz];
		uint8_t m_flag;
	};

	struct ShortLayout
	{
		MidoriValue m_buffer[SOO_CAPACITY];
		uint8_t m_padding[STORAGE_SIZE - SOO_CAPACITY * sizeof(MidoriValue) - 1uz];
		uint8_t m_size_flag;
	};

	static_assert(sizeof(LongLayout) == STORAGE_SIZE);
	static_assert(sizeof(ShortLayout) == STORAGE_SIZE);
	static_assert(offsetof(LongLayout, m_flag) == offsetof(ShortLayout, m_size_flag));

	union
	{
		LongLayout m_long;
		ShortLayout m_short;
	};

public:
	MidoriTuple();

	MidoriTuple(int size);

	MidoriTuple(const MidoriTuple& other);

	MidoriTuple(MidoriTuple&& other) noexcept;

	MidoriTuple& operator=(const MidoriTuple& other);

	MidoriTuple& operator=(MidoriTuple&& other) noexcept;

	~MidoriTuple();

	MIDORI_FORCE_INLINE MidoriValue& operator[](int index)
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	MIDORI_FORCE_INLINE const MidoriValue& operator[](int index) const
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	MIDORI_FORCE_INLINE int GetLength() const
	{
		return IsShort() ? GetShortSize() : m_long.m_size;
	}

	size_t GetCapacity() const;

private:
	MIDORI_FORCE_INLINE bool IsShort() const noexcept
	{
		return (m_short.m_size_flag & 1) != 0;
	}

	MIDORI_FORCE_INLINE void SetShortSize(int size)
	{
		m_short.m_size_flag = static_cast<uint8_t>((size << 1) | 1);
	}

	MIDORI_FORCE_INLINE int GetShortSize() const
	{
		return m_short.m_size_flag >> 1;
	}
};

class MidoriIntRange
{
private:
	MidoriInteger m_start;
	MidoriInteger m_end;
	MidoriInteger m_step;

public:
	MidoriIntRange() = default;

	MidoriIntRange(MidoriInteger start, MidoriInteger end, MidoriInteger step);

	MidoriInteger GetStart() const;

	MidoriInteger GetEnd() const;

	MidoriInteger GetStep() const;
};

class MidoriFloatRange
{
private:
	MidoriFloat m_start;
	MidoriFloat m_end;
	MidoriFloat m_step;

public:
	MidoriFloatRange() = default;

	MidoriFloatRange(MidoriFloat start, MidoriFloat end, MidoriFloat step);

	MidoriFloat GetStart() const;

	MidoriFloat GetEnd() const;

	MidoriFloat GetStep() const;
};

struct MidoriCellValue
{
	MidoriValue m_value;

	MidoriCellValue() noexcept;

	explicit MidoriCellValue(MidoriValue value) noexcept;

	MidoriValue& GetValue();

	const MidoriValue& GetValue() const;
};

// A user-visible Cell<T>. Deliberately a different traceable type from
// MidoriCellValue: GET_LOCAL_CELL and SET_LOCAL_CELL decide whether a local slot
// was boxed for capture by checking for a MidoriCellValue pointer, so a user cell
// of that type sitting unboxed in a local would be unwrapped by mistake.
struct MidoriMutableCell
{
	MidoriValue m_value;

	explicit MidoriMutableCell(MidoriValue value) noexcept;
};

struct MidoriClosure
{
	MidoriTuple m_cell_values;
	int m_proc_index;
};

struct MidoriStruct
{
	MidoriTuple m_values{};
};

struct MidoriUnion
{
	MidoriTuple m_values{};
	int m_index{ 0 };
};

class MidoriTraceable
{
public:
	enum class TraceableType : uint8_t
	{
		Text,
		Array,
		Tuple,
		IntRange,
		FloatRange,
		Struct,
		Union,
		Cell,
		MutableCell,
		Closure
	};

private:
	union
	{
		MidoriText m_text;
		MidoriArray m_array;
		MidoriTuple m_tuple;
		MidoriIntRange m_int_range;
		MidoriFloatRange m_float_range;
		MidoriStruct m_struct;
		MidoriUnion m_union;
		MidoriCellValue m_cell;
		MidoriMutableCell m_mutable_cell;
		MidoriClosure m_closure;
	};
	TraceableType m_type;

	template<typename T>
	static constexpr TraceableType TypeToEnum()
	{
		if constexpr (std::is_same_v<T, MidoriText>)
		{
			return TraceableType::Text;
		}
		else if constexpr (std::is_same_v<T, MidoriArray>)
		{
			return TraceableType::Array;
		}
		else if constexpr (std::is_same_v<T, MidoriTuple>)
		{
			return TraceableType::Tuple;
		}
		else if constexpr (std::is_same_v<T, MidoriIntRange>)
		{
			return TraceableType::IntRange;
		}
		else if constexpr (std::is_same_v<T, MidoriFloatRange>)
		{
			return TraceableType::FloatRange;
		}
		else if constexpr (std::is_same_v<T, MidoriStruct>)
		{
			return TraceableType::Struct;
		}
		else if constexpr (std::is_same_v<T, MidoriUnion>)
		{
			return TraceableType::Union;
		}
		else if constexpr (std::is_same_v<T, MidoriCellValue>)
		{
			return TraceableType::Cell;
		}
		else if constexpr (std::is_same_v<T, MidoriMutableCell>)
		{
			return TraceableType::MutableCell;
		}
		else if constexpr (std::is_same_v<T, MidoriClosure>)
		{
			return TraceableType::Closure;
		}
		else
		{
			static_assert(std::is_same_v<T, void>, "Invalid type for MidoriTraceable");
		}
	}

public:

	template<typename T>
	constexpr bool IsTraceable()
	{
		return m_type == TypeToEnum<T>();
	}

	template<typename T>
	constexpr T& GetTraceable()
	{
		if constexpr (std::is_same_v<T, MidoriText>)
		{
			return m_text;
		}
		else if constexpr (std::is_same_v<T, MidoriArray>)
		{
			return m_array;
		}
		else if constexpr (std::is_same_v<T, MidoriTuple>)
		{
			return m_tuple;
		}
		else if constexpr (std::is_same_v<T, MidoriIntRange>)
		{
			return m_int_range;
		}
		else if constexpr (std::is_same_v<T, MidoriFloatRange>)
		{
			return m_float_range;
		}
		else if constexpr (std::is_same_v<T, MidoriStruct>)
		{
			return m_struct;
		}
		else if constexpr (std::is_same_v<T, MidoriUnion>)
		{
			return m_union;
		}
		else if constexpr (std::is_same_v<T, MidoriCellValue>)
		{
			return m_cell;
		}
		else if constexpr (std::is_same_v<T, MidoriMutableCell>)
		{
			return m_mutable_cell;
		}
		else if constexpr (std::is_same_v<T, MidoriClosure>)
		{
			return m_closure;
		}
	}

	size_t GetSize() const;

#if MIDORI_DEBUG_FULL
	MidoriText ToText();
#endif

	~MidoriTraceable();

	static void operator delete(void* object, size_t size) noexcept;
	static void operator delete(void* object, std::align_val_t al) noexcept;

	static void* operator new(size_t size) noexcept;
	static void* operator new(size_t size, std::align_val_t al) noexcept;
	static void* operator new(size_t, void* ptr) noexcept;
	static void* operator new(size_t, std::align_val_t, void* ptr) noexcept; 
	static void operator delete(void*, void*) noexcept;
	static void operator delete(void*, std::align_val_t, void*) noexcept;

	MidoriTraceable(MidoriText&& str) noexcept;
	MidoriTraceable(MidoriArray&& array) noexcept;
	MidoriTraceable(MidoriTuple&& tuple) noexcept;
	MidoriTraceable(MidoriIntRange&& range) noexcept;
	MidoriTraceable(MidoriFloatRange&& range) noexcept;
	MidoriTraceable(MidoriCellValue&& cell_value) noexcept;
	MidoriTraceable(MidoriMutableCell&& mutable_cell) noexcept;
	MidoriTraceable(MidoriClosure&& closure) noexcept;
	MidoriTraceable(MidoriStruct&& midori_struct) noexcept;
	MidoriTraceable(MidoriUnion&& midori_union) noexcept;

private:
	MidoriTraceable() = delete;
	MidoriTraceable(const MidoriTraceable& other) = delete;
	MidoriTraceable(MidoriTraceable&& other) noexcept = delete;
	MidoriTraceable& operator=(const MidoriTraceable& other) = delete;
	MidoriTraceable& operator=(MidoriTraceable&& other) noexcept = delete;
};
