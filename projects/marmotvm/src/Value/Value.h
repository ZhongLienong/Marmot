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

class MidoriTraceable;
class MidoriText;

using MidoriUnit = std::monostate;
using MidoriBool = bool;

class MidoriValue
{
public:
	constexpr static inline int DATA_BUFFER_SIZE = sizeof(double);

	// sizeof(MidoriValue) as compiled inside MarmotRuntime. Out of line on purpose,
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

	MIDORI_FORCE_INLINE MidoriBool GetBool() const noexcept
	{
		return m_data.m_bool;
	}

	MIDORI_FORCE_INLINE MidoriTraceable* GetPointer() const noexcept
	{
		return m_data.m_pointer;
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
	// Sized so MidoriTraceable fills the largest allocator slot; small payloads then
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

	// The text's bytes, with no promise of a NUL after them.
	MIDORI_FORCE_INLINE std::string_view View() const noexcept
	{
		return IsShort() ? std::string_view(m_short.m_buffer, static_cast<size_t>(GetShortSize())) : std::string_view(m_long.m_ptr, static_cast<size_t>(m_long.m_size));
	}

	// The text's bytes followed by a NUL, for what reads a C string: a
	// foreign function, a C API.
	const char* CString();

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

	// The number the whole text spells, or nothing: "12abc", " 12" and "" are
	// not numbers.
	std::optional<MidoriInteger> ParseInteger() const;

	std::optional<MidoriFloat> ParseFloat() const;

	static MidoriText FromInteger(MidoriInteger value);

	static MidoriText FromWord(MidoriWord value);

	static MidoriText FromFloat(MidoriFloat value);

	static MidoriText Concatenate(const MidoriText& a, const MidoriText& b);

	static MidoriText FromFFI(char* ffi_allocated_string);

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

	MidoriText& AppendBytes(std::string_view bytes, int length);

	MidoriText& PrependBytes(std::string_view bytes);

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
	// Sized so MidoriTraceable fills the largest allocator slot; small payloads then
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

// A user-visible Ref<T>.
struct MidoriMutableCell
{
	MidoriValue m_value;

	explicit MidoriMutableCell(MidoriValue value) noexcept;
};

// Every heap object starts with the same 8-byte header, so a match reads its tag
// on the same cache line as its first fields, and is allocated in a slot sized
// for it rather than the largest. An aggregate (a tuple, struct, union or
// closure) holds its values right after the header when they fit in the
// largest slot, and in a buffer of its own when they do not.
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
		MutableCell,
		Closure
	};

	static constexpr size_t HEADER_SIZE = 8uz;
	static constexpr int INLINE_CAPACITY = static_cast<int>(sizeof(MidoriText) / sizeof(MidoriValue));

	// The bytes an aggregate of `length` values takes in its slot.
	static constexpr size_t AggregateBytes(int length) noexcept
	{
		return HEADER_SIZE + (length > 0 && length <= INLINE_CAPACITY ? static_cast<size_t>(length) * sizeof(MidoriValue) : sizeof(MidoriValue*));
	}

private:
	TraceableType m_type;
	uint8_t m_padding;
	// A union's tag or a closure's procedure.
	uint16_t m_index;
	// An aggregate's value count.
	uint32_t m_length;
	union
	{
		MidoriText m_text;
		MidoriArray m_array;
		MidoriIntRange m_int_range;
		MidoriFloatRange m_float_range;
		MidoriMutableCell m_mutable_cell;
		MidoriValue m_inline_values[INLINE_CAPACITY];
		MidoriValue* m_external_values;
	};

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
		else if constexpr (std::is_same_v<T, MidoriIntRange>)
		{
			return TraceableType::IntRange;
		}
		else if constexpr (std::is_same_v<T, MidoriFloatRange>)
		{
			return TraceableType::FloatRange;
		}
		else if constexpr (std::is_same_v<T, MidoriMutableCell>)
		{
			return TraceableType::MutableCell;
		}
		else
		{
			static_assert(std::is_same_v<T, void>, "Invalid type for MidoriTraceable");
		}
	}

public:

	MIDORI_FORCE_INLINE TraceableType GetType() const noexcept
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
		if constexpr (std::is_same_v<T, MidoriText>)
		{
			return m_text;
		}
		else if constexpr (std::is_same_v<T, MidoriArray>)
		{
			return m_array;
		}
		else if constexpr (std::is_same_v<T, MidoriIntRange>)
		{
			return m_int_range;
		}
		else if constexpr (std::is_same_v<T, MidoriFloatRange>)
		{
			return m_float_range;
		}
		else if constexpr (std::is_same_v<T, MidoriMutableCell>)
		{
			return m_mutable_cell;
		}
	}

	MIDORI_FORCE_INLINE int GetIndex() const noexcept
	{
		return static_cast<int>(m_index);
	}

	MIDORI_FORCE_INLINE int GetLength() const noexcept
	{
		return static_cast<int>(m_length);
	}

	// An aggregate's GetLength() values. A pointer rather than a span: the
	// dispatch loop and the collector read through it, and Debug builds do not
	// inline span's members.
	MIDORI_FORCE_INLINE MidoriValue* GetValues() noexcept
	{
		return m_length <= static_cast<uint32_t>(INLINE_CAPACITY) ? m_inline_values : m_external_values;
	}

	MIDORI_FORCE_INLINE const MidoriValue* GetValues() const noexcept
	{
		return m_length <= static_cast<uint32_t>(INLINE_CAPACITY) ? m_inline_values : m_external_values;
	}

	// The bytes of the slot this object takes, and of any buffer it owns.
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
	MidoriTraceable(MidoriIntRange&& range) noexcept;
	MidoriTraceable(MidoriFloatRange&& range) noexcept;
	MidoriTraceable(MidoriMutableCell&& mutable_cell) noexcept;
	// A tuple, struct, union or closure; `index` is a union's tag or a closure's procedure.
	MidoriTraceable(TraceableType type, std::span<const MidoriValue> values, int index) noexcept;

private:
	MIDORI_NOINLINE void InitializeExternalValues(std::span<const MidoriValue> values);

	MidoriTraceable() = delete;
	MidoriTraceable(const MidoriTraceable& other) = delete;
	MidoriTraceable(MidoriTraceable&& other) noexcept = delete;
	MidoriTraceable& operator=(const MidoriTraceable& other) = delete;
	MidoriTraceable& operator=(MidoriTraceable&& other) noexcept = delete;
};

template<typename T>
concept MidoriTraceablePayload = std::same_as<T, MidoriText> || std::same_as<T, MidoriArray> || std::same_as<T, MidoriIntRange> || std::same_as<T, MidoriFloatRange> || std::same_as<T, MidoriMutableCell>;
