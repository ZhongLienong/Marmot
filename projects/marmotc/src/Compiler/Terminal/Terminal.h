#pragma once

#include <print>
#include <string_view>

namespace CompilerTerminal
{
	enum class Color
	{
		RED,
		YELLOW,
		BLUE,
		MAGENTA,
		CYAN,
		WHITE,
		BRIGHT_RED,
		BRIGHT_GREEN,
		BRIGHT_YELLOW,
		BRIGHT_CYAN,
		DARK_GRAY,
	};

	inline constexpr std::string_view RESET = "\033[0m";
	inline constexpr std::string_view BOLD = "\033[1m";

	[[nodiscard]] std::string_view Code(Color color);

	template<Color color = Color::WHITE>
	void Print(std::string_view message)
	{
		std::print("{}{}{}", Code(color), message, RESET);
	}
}
