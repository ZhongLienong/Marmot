#include "Terminal.h"

#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
	std::string_view Environment(const char* name)
	{
		const char* value = std::getenv(name);
		return value == nullptr ? std::string_view() : std::string_view(value);
	}

	bool StdoutIsTerminal()
	{
#ifdef _WIN32
		return _isatty(_fileno(stdout)) != 0;
#else
		return isatty(fileno(stdout)) != 0;
#endif
	}
}

namespace RuntimeTerminal
{
	bool ColorEnabled()
	{
		static const bool s_enabled = Environment("NO_COLOR").empty()
			&& ((!Environment("CLICOLOR_FORCE").empty() && Environment("CLICOLOR_FORCE") != "0") || StdoutIsTerminal());
		return s_enabled;
	}

	std::string_view Code(Color color)
	{
		if (!ColorEnabled())
		{
			return {};
		}

		switch (color)
		{
		case Color::RESET:
			return "\033[0m";
		case Color::BOLD:
			return "\033[1m";
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
