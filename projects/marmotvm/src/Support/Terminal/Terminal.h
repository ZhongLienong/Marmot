#pragma once

#include <print>
#include <string_view>

namespace RuntimeTerminal
{
	enum class Color
	{
		RESET,
		BOLD,
		RED,
		GREEN,
		YELLOW,
		BLUE,
		WHITE,
		BRIGHT_RED,
		BRIGHT_YELLOW,
		BRIGHT_CYAN,
		BRIGHT_WHITE,
	};

	// Runtime errors go to stdout, so they are coloured when it is a terminal.
	// NO_COLOR turns colour off and CLICOLOR_FORCE on, NO_COLOR first.
	[[nodiscard]] bool ColorEnabled();

	// The escape code for `color`, or nothing when colour is off.
	[[nodiscard]] std::string_view Code(Color color);

	template<Color color = Color::WHITE>
	void Print(std::string_view message)
	{
		std::print("{}{}{}", Code(color), message, Code(Color::RESET));
	}
}
