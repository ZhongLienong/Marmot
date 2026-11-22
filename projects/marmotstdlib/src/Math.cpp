#include "MarmotStdLibExports.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <random>

namespace
{
	double ReadDouble(void** args, size_t index = 0u)
	{
		double data;
		std::memcpy(&data, &args[index], sizeof(double));
		return data;
	}

	void WriteDouble(void* ret, double value)
	{
		std::memcpy(ret, &value, sizeof(double));
	}

	void WriteInt(void* ret, int64_t value)
	{
		std::memcpy(ret, &value, sizeof(int64_t));
	}

	std::mt19937_64& GetRng()
	{
		thread_local std::mt19937_64 s_rng(std::random_device{}());
		return s_rng;
	}
}

extern "C"
{
	// Basic math
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(SquareRoot)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::sqrt(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Abs)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::abs(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Pow)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::pow(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Exp)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::exp(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Log)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::log(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Log10)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::log10(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Log2)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::log2(ReadDouble(args)));
	}

	// Trigonometric
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Sin)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::sin(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Cos)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::cos(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Tan)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::tan(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Asin)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::asin(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Acos)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::acos(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Atan)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::atan(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Atan2)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::atan2(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	// Hyperbolic
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Sinh)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::sinh(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Cosh)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::cosh(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Tanh)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::tanh(ReadDouble(args)));
	}

	// Rounding
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Floor)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::floor(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Ceil)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::ceil(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Round)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::round(ReadDouble(args)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Trunc)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::trunc(ReadDouble(args)));
	}

	// Other
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Min)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::min(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Max)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::max(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Clamp)(void** args, void* ret) noexcept
	{
		const double value = ReadDouble(args, 0u);
		const double min_val = ReadDouble(args, 1u);
		const double max_val = ReadDouble(args, 2u);
		WriteDouble(ret, std::clamp(value, min_val, max_val));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Sign)(void** args, void* ret) noexcept
	{
		const double value = ReadDouble(args);
		const double result = (value > 0.0) ? 1.0 : ((value < 0.0) ? -1.0 : 0.0);
		WriteDouble(ret, result);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Fmod)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::fmod(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Hypot)(void** args, void* ret) noexcept
	{
		WriteDouble(ret, std::hypot(ReadDouble(args, 0u), ReadDouble(args, 1u)));
	}

	// Random
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Random)(void**, void* ret) noexcept
	{
		std::uniform_real_distribution<double> dist(0.0, 1.0);
		WriteDouble(ret, dist(GetRng()));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(RandomInt)(void** args, void* ret) noexcept
	{
		int64_t minimum;
		int64_t maximum;
		std::memcpy(&minimum, &args[0u], sizeof(minimum));
		std::memcpy(&maximum, &args[1u], sizeof(maximum));
		if (minimum > maximum)
		{
			std::swap(minimum, maximum);
		}
		std::uniform_int_distribution<int64_t> dist(minimum, maximum);
		WriteInt(ret, dist(GetRng()));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(RandomFloat)(void** args, void* ret) noexcept
	{
		const double min_val = ReadDouble(args, 0u);
		const double max_val = ReadDouble(args, 1u);
		std::uniform_real_distribution<double> dist(min_val, max_val);
		WriteDouble(ret, dist(GetRng()));
	}

	// Conversion
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ToRadians)(void** args, void* ret) noexcept
	{
		constexpr double deg_to_rad = 3.14159265358979323846 / 180.0;
		WriteDouble(ret, ReadDouble(args) * deg_to_rad);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ToDegrees)(void** args, void* ret) noexcept
	{
		constexpr double rad_to_deg = 180.0 / 3.14159265358979323846;
		WriteDouble(ret, ReadDouble(args) * rad_to_deg);
	}

	// Special functions
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(IsNaN)(void** args, void* ret) noexcept
	{
		const int64_t result = std::isnan(ReadDouble(args)) ? 1 : 0;
		WriteInt(ret, result);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(IsInf)(void** args, void* ret) noexcept
	{
		const int64_t result = std::isinf(ReadDouble(args)) ? 1 : 0;
		WriteInt(ret, result);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(IsFinite)(void** args, void* ret) noexcept
	{
		const int64_t result = std::isfinite(ReadDouble(args)) ? 1 : 0;
		WriteInt(ret, result);
	}
}

