#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace RuntimeTerminal
{
	enum class Color
	{
		RESET,
		BOLD,
		RED,
		BLUE,
		WHITE,
		CYAN,
		BRIGHT_RED,
		BRIGHT_YELLOW,
		BRIGHT_CYAN,
		BRIGHT_WHITE,
		DARK_GRAY,
	};

	// Runtime errors go to stdout, so they are coloured when it is a terminal.
	// NO_COLOR turns colour off and CLICOLOR_FORCE on, NO_COLOR first.
	[[nodiscard]] bool ColorEnabled();

	// The escape code for `color`, or nothing when colour is off.
	[[nodiscard]] std::string_view Code(Color color);

	template<Color color>
	[[nodiscard]] std::string Colored(std::string_view text)
	{
		return std::format("{}{}{}", Code(color), text, Code(Color::RESET));
	}

	template<Color color = Color::WHITE>
	void Print(std::FILE* output, std::string_view message)
	{
		std::print(output, "{}", Colored<color>(message));
	}
}
