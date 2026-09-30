#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

// Int arithmetic wraps in two's complement. Signed overflow is undefined in
// C++, so each operation goes through the unsigned type, and the VM and the
// constant folder share these so a folded expression and a computed one agree.
namespace MidoriIntegerArithmetic
{
	using Signed = int64_t;
	using Unsigned = uint64_t;

	constexpr Signed Add(Signed left, Signed right) noexcept
	{
		return static_cast<Signed>(static_cast<Unsigned>(left) + static_cast<Unsigned>(right));
	}

	constexpr Signed Subtract(Signed left, Signed right) noexcept
	{
		return static_cast<Signed>(static_cast<Unsigned>(left) - static_cast<Unsigned>(right));
	}

	constexpr Signed Multiply(Signed left, Signed right) noexcept
	{
		return static_cast<Signed>(static_cast<Unsigned>(left) * static_cast<Unsigned>(right));
	}

	constexpr Signed Negate(Signed value) noexcept
	{
		return static_cast<Signed>(Unsigned{ 0u } - static_cast<Unsigned>(value));
	}

	// The divisor is not zero: that is a runtime error raised before this. The
	// one quotient that overflows, minimum / -1, traps in hardware.
	constexpr Signed Divide(Signed left, Signed right) noexcept
	{
		return right == -1 ? Negate(left) : left / right;
	}

	constexpr Signed Remainder(Signed left, Signed right) noexcept
	{
		return right == -1 ? 0 : left % right;
	}

	// A shift by the width or more moves every bit out: zero, or the sign for an
	// arithmetic right shift. A negative count is a huge one.
	template<typename Integer>
	constexpr Integer ShiftLeft(Integer value, Unsigned count) noexcept
	{
		using Bits = std::make_unsigned_t<Integer>;
		constexpr Unsigned width = std::numeric_limits<Bits>::digits;
		return count >= width ? Integer{ 0 } : static_cast<Integer>(static_cast<Bits>(static_cast<Bits>(value) << count));
	}

	template<typename Integer>
	constexpr Integer ShiftRight(Integer value, Unsigned count) noexcept
	{
		constexpr Unsigned width = std::numeric_limits<std::make_unsigned_t<Integer>>::digits;
		if (count >= width)
		{
			return value < Integer{ 0 } ? static_cast<Integer>(-1) : Integer{ 0 };
		}
		return static_cast<Integer>(value >> count);
	}
}
