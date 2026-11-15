#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "Support/Attributes/Attributes.h"
#include "Bytecode/Scalar/Scalar.h"

class MarmotTraceable;
class MarmotText;

using MarmotUnit = std::monostate;
using MarmotBool = bool;

class MarmotValue
{
public:
	constexpr static inline int DATA_BUFFER_SIZE = sizeof(double);

	// sizeof(MarmotValue) as compiled inside MarmotRuntime. Out of line on purpose,
	// so a consumer can compare it with its own sizeof(MarmotValue): the layout
	// below depends on MARMOT_DEBUG_FULL, and a mismatch means objects passed
	// across the library boundary are read with the wrong size.
	[[nodiscard]] static std::size_t LibrarySize() noexcept;
private:
	union
	{
		MarmotFloat m_float;
		MarmotInteger m_integer;
		MarmotBool m_bool;
		MarmotTraceable* m_pointer;
	} m_data;
#if MARMOT_DEBUG_FULL
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
	MARMOT_FORCE_INLINE MarmotValue() noexcept
		: m_data{.m_integer = 0}
#if MARMOT_DEBUG_FULL
		, m_tag(UNIT)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotFloat d) noexcept
		: m_data{.m_float = d}
#if MARMOT_DEBUG_FULL
		, m_tag(FLOAT)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotInteger l) noexcept
		: m_data{.m_integer = l}
#if MARMOT_DEBUG_FULL
		, m_tag(INT)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotByte byte) noexcept
		: m_data{.m_integer = static_cast<MarmotInteger>(byte)}
#if MARMOT_DEBUG_FULL
		, m_tag(BYTE)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotWord word) noexcept
		: m_data{.m_integer = static_cast<MarmotInteger>(word)}
#if MARMOT_DEBUG_FULL
		, m_tag(WORD)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotBool b) noexcept
		: m_data{.m_bool = b}
#if MARMOT_DEBUG_FULL
		, m_tag(BOOL)
#endif
	{
	}

	MARMOT_FORCE_INLINE MarmotValue(MarmotTraceable* o) noexcept
		: m_data{.m_pointer = o}
#if MARMOT_DEBUG_FULL
		, m_tag(POINTER)
#endif
	{
	}

	MarmotValue(const MarmotValue& other) noexcept = default;

	MarmotValue(MarmotValue&& other) noexcept = default;

	MarmotValue& operator=(const MarmotValue& other) noexcept = default;

	MarmotValue& operator=(MarmotValue&& other) noexcept = default;

	MARMOT_FORCE_INLINE MarmotFloat GetFloat() const noexcept
	{
		return m_data.m_float;
	}

	MARMOT_FORCE_INLINE MarmotInteger GetInteger() const noexcept
	{
		return m_data.m_integer;
	}

	MARMOT_FORCE_INLINE MarmotByte GetByte() const noexcept
	{
		return static_cast<MarmotByte>(m_data.m_integer & 0xFF);
	}

	MARMOT_FORCE_INLINE MarmotWord GetWord() const noexcept
	{
		return static_cast<MarmotWord>(m_data.m_integer);
	}

	MARMOT_FORCE_INLINE MarmotBool GetBool() const noexcept
	{
		return m_data.m_bool;
	}

	MARMOT_FORCE_INLINE MarmotTraceable* GetPointer() const noexcept
	{
		return m_data.m_pointer;
	}

	MARMOT_FORCE_INLINE MarmotWord GetRawBits() const noexcept
	{
		static_assert(sizeof(MarmotWord) == sizeof(m_data));
		MarmotWord bits = 0u;
		std::memcpy(&bits, &m_data, sizeof(bits));
		return bits;
	}

	static MARMOT_FORCE_INLINE MarmotValue FromRawBits(MarmotWord bits) noexcept
	{
		MarmotValue value;
		static_assert(sizeof(MarmotWord) == sizeof(value.m_data));
		std::memcpy(&value.m_data, &bits, sizeof(bits));
#if MARMOT_DEBUG_FULL
		value.m_tag = UNKNOWN;
#endif
		return value;
	}

#if MARMOT_DEBUG_FULL
	MarmotText ToText() const;

	bool IsPointer() const noexcept;

	DebugTypeTag GetTag() const noexcept;

	// Whether the tag agrees with the kind the compiler gave the value. A pointer
	// nothing has been stored in yet is the zero a placeholder holds, and a value
	// rebuilt from raw bits has no tag to compare.
	bool AgreesWithKind(bool is_pointer) const noexcept;
#endif
};

template<typename... Args>
concept MarmotValueConstructible = std::constructible_from<MarmotValue, Args...>;

template<typename T>
concept MarmotTraceableConstructible = std::constructible_from<MarmotTraceable, T>;

template <typename T>
concept MarmotNumeric = std::same_as<T, MarmotFloat> || std::same_as<T, MarmotInteger> || std::same_as<T, MarmotByte> || std::same_as<T, MarmotWord>;

class MarmotText
{
private:
	// Sized so MarmotTraceable fills the largest allocator slot; small payloads then
	// never need a separate heap buffer.
	static constexpr int STORAGE_SIZE = 72;
	static constexpr int SSO_CAPACITY = STORAGE_SIZE - 2;

	// A long text's characters follow this header in a buffer that texts
	// share: `a ++ b` writes b after a in place when a ends where the buffer
	// has been written to, and the result shares the buffer with a, so a
	// chain of appends is linear. Each text sees only its own m_size bytes,
	// so texts stay immutable; the byte at m_written is always a NUL.
	//
	// The count is not atomic because a buffer never leaves its thread: the
	// buffer pool is thread_local, each VM makes its own texts from the
	// executable's string pool, and worker transfer copies text by value
	// (ValueTransfer). Keep it that way.
	struct Buffer
	{
		int m_references;
		int m_written;
		int m_capacity;
		int m_padding;
	};

	struct LongLayout
	{
		char* m_ptr;
		int m_size;
		mutable int m_length_cache;
		// The bytes of the buffer this text allocated, which the collector
		// counts against it; a text that only shares a buffer counts none.
		int m_owned_bytes;
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
	MarmotText();

	MarmotText(const char* str);

	MarmotText(const MarmotText& other);

	MarmotText(MarmotText&& other) noexcept;

	MarmotText& operator=(const MarmotText& other);

	MarmotText& operator=(MarmotText&& other) noexcept;

	~MarmotText();

	int GetLength() const noexcept;

	MARMOT_FORCE_INLINE int GetByteLength() const noexcept
	{
		return IsShort() ? GetShortSize() : m_long.m_size;
	}

	// The text's bytes, with no promise of a NUL after them.
	MARMOT_FORCE_INLINE std::string_view View() const noexcept
	{
		return IsShort() ? std::string_view(m_short.m_buffer, static_cast<size_t>(GetShortSize())) : std::string_view(m_long.m_ptr, static_cast<size_t>(m_long.m_size));
	}

	// The text's bytes followed by a NUL, for what reads a C string: a
	// foreign function, a C API.
	const char* CString();

	MarmotText& Pop();

	MarmotText& Append(const char* str);

	MarmotText& Append(char c);

	MarmotText& Append(const MarmotText& other);

	void Reserve(int capacity);

	MarmotText& Prepend(const char* str);

	MarmotText& Prepend(char c);

	MarmotText& Prepend(const MarmotText& other);

	MarmotText Substring(int start, int end) const;

	std::vector<MarmotText> Split(const MarmotText& delimiter) const;

	MarmotText Reverse() const;

	// Every code point, in one pass. A byte that starts no UTF-8 sequence is
	// one code point of its own value, as GetLength counts it.
	std::vector<MarmotInteger> CodePoints() const;

	// The caller has checked each is a Unicode scalar value other than U+0000.
	static MarmotText FromCodePoints(std::span<const MarmotInteger> code_points);

	bool Contains(const MarmotText& other) const;

	MarmotText Replace(const MarmotText& old_value, const MarmotText& new_value) const;

	MarmotText Trim() const;

	char operator[](int index) const;

	bool operator==(const MarmotText& other) const;

	bool operator!=(const MarmotText& other) const;

	// The number the whole text spells, or nothing: "12abc", " 12" and "" are
	// not numbers.
	std::optional<MarmotInteger> ParseInteger() const;

	std::optional<MarmotFloat> ParseFloat() const;

	static MarmotText FromInteger(MarmotInteger value);

	static MarmotText FromWord(MarmotWord value);

	static MarmotText FromFloat(MarmotFloat value);

	static MarmotText Concatenate(const MarmotText& a, const MarmotText& b);

	static MarmotText FromFFI(char* ffi_allocated_string);

	size_t GetOwnedBytes() const;

private:
	Buffer& GetBuffer() const noexcept;

	// Whether `size` bytes fit in the buffer after this text's own, with no
	// other text's bytes there.
	bool CanWriteAtEnd(int size) const noexcept;

	// Whether no other text sees the buffer, so its bytes may change.
	bool IsPrivate() const noexcept;

	// Moves the text to a buffer of its own with room for `capacity` bytes.
	void Reallocate(int capacity);

	void Release() noexcept;

	MarmotText& AppendBytes(std::string_view bytes, int length);

	MarmotText& PrependBytes(std::string_view bytes);

	MARMOT_FORCE_INLINE bool IsShort() const noexcept
	{
		return (m_short.m_size_flag & 1) != 0;
	}

	MARMOT_FORCE_INLINE void SetShortSize(int size)
	{
		m_short.m_size_flag = static_cast<uint8_t>((size << 1) | 1);
	}

	MARMOT_FORCE_INLINE int GetShortSize() const
	{
		return m_short.m_size_flag >> 1;
	}
};

class MarmotArray
{
private:
	static constexpr int s_initial_capacity = 8;
	// Sized so MarmotTraceable fills the largest allocator slot; small payloads then
	// never need a separate heap buffer.
	static constexpr int STORAGE_SIZE = 72;
	static constexpr int SOO_CAPACITY = (STORAGE_SIZE - 8) / static_cast<int>(sizeof(MarmotValue));

	struct LongLayout
	{
		MarmotValue* m_ptr;
		int m_size;
		int m_capacity;
		uint8_t m_padding[STORAGE_SIZE - sizeof(MarmotValue*) - 2uz * sizeof(int) - 1uz];
		uint8_t m_flag;
	};

	struct ShortLayout
	{
		MarmotValue m_buffer[SOO_CAPACITY];
		uint8_t m_padding[STORAGE_SIZE - SOO_CAPACITY * sizeof(MarmotValue) - 1uz];
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
	MarmotArray();

	MarmotArray(int size);

	MarmotArray(const MarmotArray& other);

	MarmotArray(MarmotArray&& other) noexcept;

	MarmotArray& operator=(const MarmotArray& other);

	MarmotArray& operator=(MarmotArray&& other) noexcept;

	~MarmotArray();

	MARMOT_FORCE_INLINE MarmotValue& operator[](int index)
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	MARMOT_FORCE_INLINE const MarmotValue& operator[](int index) const
	{
		return IsShort() ? m_short.m_buffer[index] : m_long.m_ptr[index];
	}

	void AddBack(const MarmotValue& value);

	void Extend(const MarmotArray& other);

	MarmotArray Slice(int start, int end) const;

	MarmotArray Reverse() const;

	bool Contains(const MarmotValue& value) const;

	MARMOT_FORCE_INLINE int GetLength() const
	{
		return IsShort() ? GetShortSize() : m_long.m_size;
	}

	size_t GetCapacity() const;

	static MarmotArray Concatenate(const MarmotArray& a, const MarmotArray& b);

	static MarmotArray FromFFI(MarmotValue* ffi_allocated_data, int length);

private:
	void Expand(int new_capacity);

	MARMOT_FORCE_INLINE bool IsShort() const noexcept
	{
		return (m_short.m_size_flag & 1) != 0;
	}

	MARMOT_FORCE_INLINE void SetShortSize(int size)
	{
		m_short.m_size_flag = static_cast<uint8_t>((size << 1) | 1);
	}

	MARMOT_FORCE_INLINE int GetShortSize() const
	{
		return m_short.m_size_flag >> 1;
	}
};

class MarmotIntRange
{
private:
	MarmotInteger m_start;
	MarmotInteger m_end;
	MarmotInteger m_step;

public:
	MarmotIntRange() = default;

	MarmotIntRange(MarmotInteger start, MarmotInteger end, MarmotInteger step);

	MarmotInteger GetStart() const;

	MarmotInteger GetEnd() const;

	MarmotInteger GetStep() const;
};

class MarmotFloatRange
{
private:
	MarmotFloat m_start;
	MarmotFloat m_end;
	MarmotFloat m_step;

public:
	MarmotFloatRange() = default;

	MarmotFloatRange(MarmotFloat start, MarmotFloat end, MarmotFloat step);

	MarmotFloat GetStart() const;

	MarmotFloat GetEnd() const;

	MarmotFloat GetStep() const;
};

// A user-visible Ref<T>.
struct MarmotMutableCell
{
	MarmotValue m_value;

	explicit MarmotMutableCell(MarmotValue value) noexcept;
};

// Every heap object starts with the same 8-byte header, so a match reads its tag
// on the same cache line as its first fields, and is allocated in a slot sized
// for it rather than the largest. An aggregate (a tuple, struct, union or
// closure) holds its values right after the header when they fit in the
// largest slot, and in a buffer of its own when they do not.
class MarmotTraceable
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
		MutableCell,
		Closure
	};

	static constexpr size_t HEADER_SIZE = 8uz;
	static constexpr int INLINE_CAPACITY = static_cast<int>(sizeof(MarmotText) / sizeof(MarmotValue));

	// The bytes an aggregate of `length` values takes in its slot.
	static constexpr size_t AggregateBytes(int length) noexcept
	{
		return HEADER_SIZE + (length > 0 && length <= INLINE_CAPACITY ? static_cast<size_t>(length) * sizeof(MarmotValue) : sizeof(MarmotValue*));
	}

private:
	TraceableType m_type;
	// An array's or a cell's: whether the values it holds are pointers.
	bool m_holds_pointers;
	// A union's tag or a closure's procedure.
	uint16_t m_index;
	// An aggregate's value count.
	uint16_t m_length;
	// An aggregate's shape: which of its values are pointers.
	uint16_t m_shape;
	union
	{
		MarmotText m_text;
		MarmotArray m_array;
		MarmotIntRange m_int_range;
		MarmotFloatRange m_float_range;
		MarmotMutableCell m_mutable_cell;
		MarmotValue m_inline_values[INLINE_CAPACITY];
		MarmotValue* m_external_values;
	};

	template<typename T>
	static constexpr TraceableType TypeToEnum()
	{
		if constexpr (std::is_same_v<T, MarmotText>)
		{
			return TraceableType::Text;
		}
		else if constexpr (std::is_same_v<T, MarmotArray>)
		{
			return TraceableType::Array;
		}
		else if constexpr (std::is_same_v<T, MarmotIntRange>)
		{
			return TraceableType::IntRange;
		}
		else if constexpr (std::is_same_v<T, MarmotFloatRange>)
		{
			return TraceableType::FloatRange;
		}
		else if constexpr (std::is_same_v<T, MarmotMutableCell>)
		{
			return TraceableType::MutableCell;
		}
		else
		{
			static_assert(std::is_same_v<T, void>, "Invalid type for MarmotTraceable");
		}
	}

public:

	MARMOT_FORCE_INLINE TraceableType GetType() const noexcept
	{
		return m_type;
	}

	template<typename T>
	constexpr bool IsTraceable()
	{
		return m_type == TypeToEnum<T>();
	}

	template<typename T>
	constexpr T& GetTraceable()
	{
		if constexpr (std::is_same_v<T, MarmotText>)
		{
			return m_text;
		}
		else if constexpr (std::is_same_v<T, MarmotArray>)
		{
			return m_array;
		}
		else if constexpr (std::is_same_v<T, MarmotIntRange>)
		{
			return m_int_range;
		}
		else if constexpr (std::is_same_v<T, MarmotFloatRange>)
		{
			return m_float_range;
		}
		else if constexpr (std::is_same_v<T, MarmotMutableCell>)
		{
			return m_mutable_cell;
		}
	}

	MARMOT_FORCE_INLINE int GetIndex() const noexcept
	{
		return static_cast<int>(m_index);
	}

	MARMOT_FORCE_INLINE int GetLength() const noexcept
	{
		return static_cast<int>(m_length);
	}

	MARMOT_FORCE_INLINE bool HoldsPointers() const noexcept
	{
		return m_holds_pointers;
	}

	MARMOT_FORCE_INLINE uint16_t GetShape() const noexcept
	{
		return m_shape;
	}

	// An aggregate's GetLength() values. A pointer rather than a span: the
	// dispatch loop and the collector read through it, and Debug builds do not
	// inline span's members.
	MARMOT_FORCE_INLINE MarmotValue* GetValues() noexcept
	{
		return m_length <= static_cast<uint16_t>(INLINE_CAPACITY) ? m_inline_values : m_external_values;
	}

	MARMOT_FORCE_INLINE const MarmotValue* GetValues() const noexcept
	{
		return m_length <= static_cast<uint16_t>(INLINE_CAPACITY) ? m_inline_values : m_external_values;
	}

	// The bytes of the slot this object takes, and of any buffer it owns.
	size_t GetSize() const;

#if MARMOT_DEBUG_FULL
	MarmotText ToText();
#endif

	~MarmotTraceable();

	static void operator delete(void* object, size_t size) noexcept;
	static void operator delete(void* object, std::align_val_t al) noexcept;

	static void* operator new(size_t size) noexcept;
	static void* operator new(size_t size, std::align_val_t al) noexcept;
	static void* operator new(size_t, void* ptr) noexcept;
	static void* operator new(size_t, std::align_val_t, void* ptr) noexcept; 
	static void operator delete(void*, void*) noexcept;
	static void operator delete(void*, std::align_val_t, void*) noexcept;

	MarmotTraceable(MarmotText&& str) noexcept;
	MarmotTraceable(MarmotArray&& array, bool holds_pointers) noexcept;
	MarmotTraceable(MarmotIntRange&& range) noexcept;
	MarmotTraceable(MarmotFloatRange&& range) noexcept;
	MarmotTraceable(MarmotMutableCell&& mutable_cell, bool holds_pointer) noexcept;
	// A tuple, struct, union or closure; `index` is a union's tag or a closure's procedure.
	MarmotTraceable(TraceableType type, std::span<const MarmotValue> values, int index, uint16_t shape) noexcept;

private:
	MARMOT_NOINLINE void InitializeExternalValues(std::span<const MarmotValue> values);

	MarmotTraceable() = delete;
	MarmotTraceable(const MarmotTraceable& other) = delete;
	MarmotTraceable(MarmotTraceable&& other) noexcept = delete;
	MarmotTraceable& operator=(const MarmotTraceable& other) = delete;
	MarmotTraceable& operator=(MarmotTraceable&& other) noexcept = delete;
};

// A payload with no values in it, which needs no kind to be traced.
template<typename T>
concept MarmotScalarPayload = std::same_as<T, MarmotText> || std::same_as<T, MarmotIntRange> || std::same_as<T, MarmotFloatRange>;
