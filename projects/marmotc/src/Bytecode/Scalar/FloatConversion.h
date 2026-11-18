#pragma once

#include "Scalar.h"

#include <cmath>
#include <limits>
#include <optional>

namespace MarmotFloatConversion
{
	template<typename Integer>
	std::optional<Integer> ToInteger(MarmotFloat value) noexcept
	{
		constexpr int digits = std::numeric_limits<Integer>::digits;
		// The exclusive upper limit is exact even when max() rounds up as a Float.
		constexpr MarmotFloat upper = 2.0 * static_cast<MarmotFloat>(uint64_t{ 1 } << (digits - 1));
		constexpr MarmotFloat lower = std::numeric_limits<Integer>::is_signed ? -upper : 0.0;
		const MarmotFloat truncated = std::trunc(value);
		if (truncated >= lower && truncated < upper)
		{
			return static_cast<Integer>(truncated);
		}
		return std::nullopt;
	}
}
