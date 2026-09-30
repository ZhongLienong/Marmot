#include "Terminal.h"

#include <utility>

namespace RuntimeTerminal
{
	std::string_view Code(Color color)
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
		case Color::WHITE:
			return "\033[37m";
		case Color::BRIGHT_RED:
			return "\033[91m";
		case Color::BRIGHT_YELLOW:
			return "\033[93m";
		case Color::BRIGHT_CYAN:
			return "\033[96m";
		case Color::BRIGHT_WHITE:
			return "\033[97m";
		}
		std::unreachable();
	}
}
