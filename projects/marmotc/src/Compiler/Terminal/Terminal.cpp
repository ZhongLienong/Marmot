#include "Terminal.h"

#include <string>
#include <utility>

namespace CompilerTerminal
{
	std::string_view Code(Color color)
	{
		switch (color)
		{
		case Color::RED:
			return "\033[31m";
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
		case Color::BRIGHT_RED:
			return "\033[91m";
		case Color::BRIGHT_GREEN:
			return "\033[92m";
		case Color::BRIGHT_YELLOW:
			return "\033[93m";
		case Color::BRIGHT_CYAN:
			return "\033[96m";
		case Color::DARK_GRAY:
			return "\033[38;5;240m";
		}
		std::unreachable();
	}

	void PrintSeparator(Color color, int width)
	{
		std::print("{}{}{}\n", Code(color), std::string(static_cast<size_t>(width), '-'), RESET);
	}
}
