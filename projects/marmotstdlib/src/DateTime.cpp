#include "MarmotStdLibExports.h"
#include "FFIAllocation/FFIAllocation.h"
#include "ProcessState.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <iomanip>
#include <sstream>

namespace
{
	using FFIAllocation::AllocateString;
	using namespace std::chrono;

	constexpr sys_days s_first_timestamp = year{0} / January / 1;
	constexpr sys_days s_after_last_timestamp = year{10000} / January / 1;
	constexpr int64_t s_invalid_timestamp = (std::numeric_limits<int64_t>::min)();

	int DecimalField(std::string_view text)
	{
		int value = 0;
		for (const char digit : text)
		{
			if (digit < '0' || digit > '9')
			{
				return -1;
			}
			value = (value * 10) + (digit - '0');
		}
		return value;
	}

	std::optional<minutes> ReadUtcOffset(std::string_view text)
	{
		if (text == "Z" || text == "z")
		{
			return minutes{0};
		}
		if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':')
		{
			return std::nullopt;
		}
		const int hour = DecimalField(text.substr(1, 2));
		const int minute = DecimalField(text.substr(4, 2));
		if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
		{
			return std::nullopt;
		}
		const minutes offset{(hour * 60) + minute};
		return text[0] == '-' ? -offset : offset;
	}

	std::optional<sys_time<milliseconds>> ReadTimestamp(std::string_view text)
	{
		if (text.size() < 20 || text[4] != '-' || text[7] != '-' ||
			(text[10] != 'T' && text[10] != 't') || text[13] != ':' || text[16] != ':')
		{
			return std::nullopt;
		}
		const int year_number = DecimalField(text.substr(0, 4));
		const int month_number = DecimalField(text.substr(5, 2));
		const int day_number = DecimalField(text.substr(8, 2));
		const int hour = DecimalField(text.substr(11, 2));
		const int minute = DecimalField(text.substr(14, 2));
		const int second = DecimalField(text.substr(17, 2));
		const year_month_day date = year{year_number} / month{static_cast<unsigned>(month_number)} / day{static_cast<unsigned>(day_number)};
		if (year_number < 0 || month_number < 1 || day_number < 1 || !date.ok() ||
			hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
		{
			return std::nullopt;
		}

		std::string_view suffix = text.substr(19);
		milliseconds subsecond{0};
		if (suffix.front() == '.')
		{
			suffix.remove_prefix(1);
			const size_t count = suffix.find_first_not_of("0123456789");
			if (count < 1 || count > 3)
			{
				return std::nullopt;
			}
			const int scale = count == 1 ? 100 : (count == 2 ? 10 : 1);
			subsecond = milliseconds{DecimalField(suffix.substr(0, count)) * scale};
			suffix.remove_prefix(count);
		}
		const std::optional<minutes> offset = ReadUtcOffset(suffix);
		if (!offset)
		{
			return std::nullopt;
		}
		// Widen the date before adding clock fields. Some duration aliases have
		// narrower representations than milliseconds.
		const sys_time<milliseconds> midnight = sys_days{date};
		const sys_time<milliseconds> timestamp = midnight + hours{hour} + minutes{minute} + seconds{second} + subsecond - *offset;
		if (timestamp < s_first_timestamp || timestamp >= s_after_last_timestamp)
		{
			return std::nullopt;
		}
		return timestamp;
	}

	std::tm ReadLocalTime()
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

	std::tm GetLocalTime()
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		return ReadLocalTime();
	}

	std::tm GetUtcTime()
	{
		// Even gmtime_r can initialize libc's time zone state from the environment.
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
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
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetTime)(void**, void* ret) noexcept
	{
		const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		const std::chrono::time_point now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
		const std::chrono::milliseconds value = now_ms.time_since_epoch();
		const std::chrono::milliseconds duration = std::chrono::duration_cast<std::chrono::milliseconds>(value);

		const double val = static_cast<double>(duration.count());
		std::memcpy(ret, &val, sizeof(double));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetMonotonicNanos)(void**, void* ret) noexcept
	{
		const int64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		std::memcpy(ret, &nanos, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetYear)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t year = static_cast<int64_t>(local_tm.tm_year + 1900);
		std::memcpy(ret, &year, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetMonth)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t month = static_cast<int64_t>(local_tm.tm_mon + 1);
		std::memcpy(ret, &month, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetDay)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day = static_cast<int64_t>(local_tm.tm_mday);
		std::memcpy(ret, &day, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetHour)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t hour = static_cast<int64_t>(local_tm.tm_hour);
		std::memcpy(ret, &hour, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetMinute)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t minute = static_cast<int64_t>(local_tm.tm_min);
		std::memcpy(ret, &minute, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetSecond)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t second = static_cast<int64_t>(local_tm.tm_sec);
		std::memcpy(ret, &second, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetDayOfWeek)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day_of_week = static_cast<int64_t>(local_tm.tm_wday);
		std::memcpy(ret, &day_of_week, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetDayOfYear)(void**, void* ret) noexcept
	{
		const std::tm local_tm = GetLocalTime();
		const int64_t day_of_year = static_cast<int64_t>(local_tm.tm_yday + 1);
		std::memcpy(ret, &day_of_year, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(FormatTime)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* format = reinterpret_cast<const char*>(args[0u]);
		const std::tm local_tm = ReadLocalTime();

		std::ostringstream oss;
		oss << std::put_time(&local_tm, format);
		char* result = AllocateString(oss.str());

		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ParseTimestamp)(void** args, void* ret) noexcept
	{
		const std::optional<std::chrono::sys_time<std::chrono::milliseconds>> timestamp =
			ReadTimestamp(static_cast<const char*>(args[0u]));
		const int64_t value = timestamp ? timestamp->time_since_epoch().count() : s_invalid_timestamp;
		std::memcpy(ret, &value, sizeof(value));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(FormatUtcTimestamp)(void** args, void* ret) noexcept
	{
		using namespace std::chrono;
		int64_t value;
		std::memcpy(&value, &args[0u], sizeof(value));
		const sys_time<milliseconds> timestamp{milliseconds{value}};
		char* result = nullptr;
		if (timestamp >= s_first_timestamp && timestamp < s_after_last_timestamp)
		{
			const sys_days date = floor<days>(timestamp);
			const year_month_day calendar{date};
			const hh_mm_ss time{timestamp - date};
			result = AllocateString(std::format(
				"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
				static_cast<int>(calendar.year()), static_cast<unsigned>(calendar.month()),
				static_cast<unsigned>(calendar.day()), time.hours().count(), time.minutes().count(),
				time.seconds().count(), time.subseconds().count()));
		}
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(ptr));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetTimezoneOffset)(void**, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
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

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcYear)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t year = static_cast<int64_t>(utc_tm.tm_year + 1900);
		std::memcpy(ret, &year, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcMonth)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t month = static_cast<int64_t>(utc_tm.tm_mon + 1);
		std::memcpy(ret, &month, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcDay)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day = static_cast<int64_t>(utc_tm.tm_mday);
		std::memcpy(ret, &day, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcHour)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t hour = static_cast<int64_t>(utc_tm.tm_hour);
		std::memcpy(ret, &hour, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcMinute)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t minute = static_cast<int64_t>(utc_tm.tm_min);
		std::memcpy(ret, &minute, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcSecond)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t second = static_cast<int64_t>(utc_tm.tm_sec);
		std::memcpy(ret, &second, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcDayOfWeek)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day_of_week = static_cast<int64_t>(utc_tm.tm_wday);
		std::memcpy(ret, &day_of_week, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetUtcDayOfYear)(void**, void* ret) noexcept
	{
		const std::tm utc_tm = GetUtcTime();
		const int64_t day_of_year = static_cast<int64_t>(utc_tm.tm_yday + 1);
		std::memcpy(ret, &day_of_year, sizeof(int64_t));
	}
}

