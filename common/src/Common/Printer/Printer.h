#pragma once

#include <string_view>
#include <concepts>
#include <print>
#include <chrono>
#include <format>

namespace Printer
{
	enum class Color
	{
		// Standard colors
		RED,			// error
		GREEN,			// success
		YELLOW,			// warning
		BLUE,			// info
		MAGENTA,		// debug
		CYAN,			// trace
		WHITE,			// default
		GRAY,			// dim info

		// Bright colors (bold variants)
		BRIGHT_RED,		// critical error
		BRIGHT_GREEN,	// important success
		BRIGHT_YELLOW,	// important warning
		BRIGHT_BLUE,	// important info
		BRIGHT_MAGENTA,	// important debug
		BRIGHT_CYAN,	// important trace
		BRIGHT_WHITE,	// highlight

		// Additional colors
		ORANGE,			// moderate warning
		PURPLE,			// special info
		DARK_GRAY,		// subtle/muted
	};

	enum class Style
	{
		NORMAL,
		BOLD,
		DIM,
		ITALIC,
		UNDERLINE,
	};

	namespace Detail
	{
		constexpr const char* GetColorCode(Color color)
		{
			switch (color)
			{
			case Color::RED:
				return "\033[31m";
			case Color::GREEN:
				return "\033[32m";
			case Color::YELLOW:
				return "\033[33m";
			case Color::BLUE:
				return "\033[34m";
			case Color::MAGENTA:
				return "\033[35m";
			case Color::CYAN:
				return "\033[36m";
			case Color::WHITE:
				return "\033[37m";
			case Color::GRAY:
				return "\033[90m";
			case Color::BRIGHT_RED:
				return "\033[91m";
			case Color::BRIGHT_GREEN:
				return "\033[92m";
			case Color::BRIGHT_YELLOW:
				return "\033[93m";
			case Color::BRIGHT_BLUE:
				return "\033[94m";
			case Color::BRIGHT_MAGENTA:
				return "\033[95m";
			case Color::BRIGHT_CYAN:
				return "\033[96m";
			case Color::BRIGHT_WHITE:
				return "\033[97m";
			case Color::ORANGE:
				return "\033[38;5;208m";
			case Color::PURPLE:
				return "\033[38;5;129m";
			case Color::DARK_GRAY:
				return "\033[38;5;240m";
			default:
				return "\033[0m";
			}
		}

		constexpr const char* GetStyleCode(Style style)
		{
			switch (style)
			{
			case Style::BOLD:
				return "\033[1m";
			case Style::DIM:
				return "\033[2m";
			case Style::ITALIC:
				return "\033[3m";
			case Style::UNDERLINE:
				return "\033[4m";
			default:
				return "";
			}
		}

		inline std::string GetTimestamp()
		{
			std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
			std::chrono::milliseconds ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
			time_t timer = std::chrono::system_clock::to_time_t(now);
			std::tm bt{};
	#ifdef _WIN32
		localtime_s(&bt, &timer);
#else
		localtime_r(&timer, &bt);
#endif

			return std::format("{:02}:{:02}:{:02}.{:03}", bt.tm_hour, bt.tm_min, bt.tm_sec, ms.count());
		}
	}

	// Basic colored print
	template<Color color = Color::WHITE>
	void Print(std::string_view message)
	{
		std::printf("%s", Detail::GetColorCode(color));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Print with style
	template<Color color = Color::WHITE, Style style = Style::NORMAL>
	void PrintStyled(std::string_view message)
	{
		std::printf("%s%s", Detail::GetStyleCode(style), Detail::GetColorCode(color));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Print with timestamp
	template<Color color = Color::WHITE>
	void PrintWithTime(std::string_view message)
	{
		std::printf("%s[%s] ", Detail::GetColorCode(Color::GRAY), Detail::GetTimestamp().c_str());
		std::printf("%s", Detail::GetColorCode(color));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Print with label prefix
	template<Color label_color = Color::CYAN, Color message_color = Color::WHITE>
	void PrintLabeled(std::string_view label, std::string_view message)
	{
		std::printf("%s[%s] ", Detail::GetColorCode(label_color), label.data());
		std::printf("%s", Detail::GetColorCode(message_color));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Print debug info with timestamp and label
	template<Color color = Color::MAGENTA>
	void DebugPrint(std::string_view label, std::string_view message)
	{
		std::printf("%s[%s] ", Detail::GetColorCode(Color::DARK_GRAY), Detail::GetTimestamp().c_str());
		std::printf("%s[%s] ", Detail::GetColorCode(color), label.data());
		std::printf("%s", Detail::GetColorCode(Color::WHITE));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Print with file and line info (for detailed debugging)
	template<Color color = Color::MAGENTA>
	void DebugPrintDetailed(std::string_view file, int line, std::string_view function, std::string_view message)
	{
		std::printf("%s[%s] ", Detail::GetColorCode(Color::DARK_GRAY), Detail::GetTimestamp().c_str());
		std::printf("%s%s:%d ", Detail::GetColorCode(Color::GRAY), file.data(), line);
		std::printf("%s%s() ", Detail::GetColorCode(Color::CYAN), function.data());
		std::printf("%s", Detail::GetColorCode(color));
		std::print("{}", message);
		std::printf("\033[0m");
	}

	// Get colored string (for building formatted output)
	template<Color color = Color::WHITE>
	inline std::string Colored(std::string_view text)
	{
		return std::string(Detail::GetColorCode(color)) + std::string(text) + "\033[0m";
	}

	// Separator line
	inline void PrintSeparator(Color color = Color::GRAY, int width = 80)
	{
		std::printf("%s", Detail::GetColorCode(color));
		for (int i = 0; i < width; i += 1)
		{
			std::printf("-");
		}
		std::printf("\033[0m\n");
	}

	// Box around text
	template<Color color = Color::CYAN>
	void PrintBox(std::string_view title, std::string_view content)
	{
		int width = static_cast<int>(std::max(title.length(), content.length())) + 4;

		std::printf("%s+", Detail::GetColorCode(color));
		for (int i = 0; i < width; i += 1)
		{
			std::printf("=");
		}
		std::printf("+\n");

		std::printf("| %s%-*s%s |\n", Detail::GetColorCode(Color::BRIGHT_WHITE), static_cast<int>(width - 2), title.data(), Detail::GetColorCode(color));

		std::printf("+");
		for (int i = 0; i < width; i += 1)
		{
			std::printf("-");
		}
		std::printf("+\n");

		std::printf("| %s%-*s%s |\n",Detail::GetColorCode(Color::WHITE), static_cast<int>(width - 2), content.data(), Detail::GetColorCode(color));

		std::printf("+");
		for (int i = 0; i < width; i += 1)
		{
			std::printf("=");
		}
		std::printf("+\033[0m\n");
	}

	// Formatted print using C++23 std::print with optional color
	template<Color color = Color::WHITE, typename... Args>
	void PrintFormatted(std::format_string<Args...> fmt, Args&&... args)
	{
		if constexpr (color != Color::WHITE)
		{
			std::printf("%s", Detail::GetColorCode(color));
		}
		std::print(fmt, std::forward<Args>(args)...);
		if constexpr (color != Color::WHITE)
		{
			std::printf("\033[0m");
		}
	}
}