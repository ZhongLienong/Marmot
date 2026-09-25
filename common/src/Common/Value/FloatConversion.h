#pragma once

#include <cstdint>
#include <limits>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

// A Float converts to an integer type by truncating toward zero. A NaN, or a value the
// type cannot hold, has no C++ conversion (static_cast is undefined for it), so these
// define one: the x86-64 truncating conversion's, which gives the minimum Int. There it is
// the instruction's own result and costs nothing; other targets check the range to agree.
namespace MidoriFloatConversion
{
	inline int64_t ToInteger(double value) noexcept
	{
#if defined(__x86_64__) || defined(_M_X64)
		return _mm_cvttsd_si64(_mm_set_sd(value));
#else
		constexpr double s_limit = 9223372036854775808.0;
		return (value >= -s_limit && value < s_limit) ? static_cast<int64_t>(value) : std::numeric_limits<int64_t>::min();
#endif
	}

	// Through Int, then wrapped as `Int as Byte` wraps.
	inline uint8_t ToByte(double value) noexcept
	{
		return static_cast<uint8_t>(static_cast<uint64_t>(ToInteger(value)));
	}

	// Exact over the whole Word range; anything else goes through Int and wraps as
	// `Int as Word` does. The one comparison is the one a compiler emits for the
	// unsigned conversion anyway.
	inline uint64_t ToWord(double value) noexcept
	{
		constexpr double s_two_to_63 = 9223372036854775808.0;
		if (value >= s_two_to_63 && value < 2.0 * s_two_to_63)
		{
			return static_cast<uint64_t>(value - s_two_to_63) + (uint64_t{ 1 } << 63);
		}
		return static_cast<uint64_t>(ToInteger(value));
	}
}
