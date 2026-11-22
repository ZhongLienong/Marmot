#pragma once

#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>

namespace MarmotTerminal
{
	enum class Color
	{
		RESET,
		BOLD,
		RED,
		YELLOW,
		BLUE,
		WHITE,
		CYAN,
		BRIGHT_RED,
		BRIGHT_YELLOW,
		BRIGHT_CYAN,
		BRIGHT_WHITE,
		DARK_GRAY,
	};

	[[nodiscard]] bool ShouldUseColor(std::FILE* output);
	void SetColorEnabled(bool enabled);
	[[nodiscard]] bool ColorEnabled();
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
