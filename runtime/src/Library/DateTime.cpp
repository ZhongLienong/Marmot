#include "Library/MidoriStdLibExports.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <string>
#include <iomanip>
#include <sstream>

namespace
{
	char* AllocateString(const std::string& str)
	{
		const size_t size = str.size() + 1u;
		char* buffer = static_cast<char*>(std::malloc(size));
		if (buffer != nullptr)
		{
			std::memcpy(buffer, str.c_str(), size);
		}
		return buffer;
	}

	std::tm GetLocalTime()
	{
		const std::time_t now = std::time(nullptr);
		std::tm local_tm{};
#ifdef _MSC_VER
		localtime_s(&local_tm, &now);
#else
		localtime_r(&now, &local_tm);
#endif
		return local_tm;
	}

	std::tm GetUtcTime()
	{
		const std::time_t now = std::time(nullptr);
		std::tm utc_tm{};
#ifdef _MSC_VER
		gmtime_s(&utc_tm, &now);
#else
		gmtime_r(&now, &utc_tm);
#endif
		return utc_tm;
	}
}

extern "C"
{
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetTime)(void**, void* ret) noexcept
	{
		const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		const std::chrono::time_point now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
		const std::chrono::milliseconds value = now_ms.time_since_epoch();
		const std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(value);

		const double val = static_cast<double>(duration.count());
		std::memcpy(ret, &val, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetYear)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t year = static_cast<int64_t>(local_tm.tm_year + 1900);
		std::memcpy(ret, &year, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetMonth)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t month = static_cast<int64_t>(local_tm.tm_mon + 1);
		std::memcpy(ret, &month, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDay)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day = static_cast<int64_t>(local_tm.tm_mday);
		std::memcpy(ret, &day, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetHour)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t hour = static_cast<int64_t>(local_tm.tm_hour);
		std::memcpy(ret, &hour, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetMinute)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t minute = static_cast<int64_t>(local_tm.tm_min);
		std::memcpy(ret, &minute, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetSecond)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t second = static_cast<int64_t>(local_tm.tm_sec);
		std::memcpy(ret, &second, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDayOfWeek)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day_of_week = static_cast<int64_t>(local_tm.tm_wday);
		std::memcpy(ret, &day_of_week, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDayOfYear)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day_of_year = static_cast<int64_t>(local_tm.tm_yday + 1);
		std::memcpy(ret, &day_of_year, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(FormatTime)(void** args, void* ret) noexcept
	{
		const char* format = reinterpret_cast<const char*>(args[0u]);
		const std::tm local_tm = GetLocalTime();

		std::ostringstream oss;
		oss << std::put_time(&local_tm, format);
		char* result = AllocateString(oss.str());

		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetTimezoneOffset)(void**, void* ret) noexcept
	{
		const std::time_t now = std::time(nullptr);
		std::tm local_tm{};
		std::tm utc_tm{};

#ifdef _MSC_VER
		localtime_s(&local_tm, &now);
		gmtime_s(&utc_tm, &now);
#else
		localtime_r(&now, &local_tm);
		gmtime_r(&now, &utc_tm);
#endif

		const std::time_t local_time = std::mktime(&local_tm);
		const std::time_t utc_time = std::mktime(&utc_tm);
		const int64_t offset_minutes = static_cast<int64_t>(std::difftime(local_time, utc_time) / 60.0);

		std::memcpy(ret, &offset_minutes, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcYear)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t year = static_cast<int64_t>(utc_tm.tm_year + 1900);
		std::memcpy(ret, &year, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcMonth)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t month = static_cast<int64_t>(utc_tm.tm_mon + 1);
		std::memcpy(ret, &month, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDay)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day = static_cast<int64_t>(utc_tm.tm_mday);
		std::memcpy(ret, &day, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcHour)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t hour = static_cast<int64_t>(utc_tm.tm_hour);
		std::memcpy(ret, &hour, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcMinute)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t minute = static_cast<int64_t>(utc_tm.tm_min);
		std::memcpy(ret, &minute, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcSecond)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t second = static_cast<int64_t>(utc_tm.tm_sec);
		std::memcpy(ret, &second, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDayOfWeek)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day_of_week = static_cast<int64_t>(utc_tm.tm_wday);
		std::memcpy(ret, &day_of_week, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDayOfYear)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day_of_year = static_cast<int64_t>(utc_tm.tm_yday + 1);
		std::memcpy(ret, &day_of_year, sizeof(int64_t));
	}
}

