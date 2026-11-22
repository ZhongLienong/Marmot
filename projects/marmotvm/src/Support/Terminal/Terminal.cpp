#include "Terminal.h"

#include <atomic>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
	std::atomic<bool> s_color_enabled = false;

	std::string_view Environment(const char* name)
	{
		const char* value = std::getenv(name);
		return value == nullptr ? std::string_view() : std::string_view(value);
	}
}

namespace MarmotTerminal
{
	bool ShouldUseColor(std::FILE* output)
	{
		if (!Environment("NO_COLOR").empty())
		{
			return false;
		}
		const std::string_view force = Environment("CLICOLOR_FORCE");
		if (!force.empty() && force != "0")
		{
			return true;
		}
		if (Environment("TERM") == "dumb")
		{
			return false;
		}
#ifdef _WIN32
		return _isatty(_fileno(output)) != 0;
#else
		return isatty(fileno(output)) != 0;
#endif
	}

	void SetColorEnabled(bool enabled)
	{
		s_color_enabled.store(enabled);
	}

	bool ColorEnabled()
	{
		return s_color_enabled.load();
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
		case Color::YELLOW:
			return "\033[33m";
		case Color::BLUE:
			return "\033[34m";
		case Color::WHITE:
			return "\033[37m";
		case Color::CYAN:
			return "\033[36m";
		case Color::BRIGHT_RED:
			return "\033[91m";
		case Color::BRIGHT_YELLOW:
			return "\033[93m";
		case Color::BRIGHT_CYAN:
			return "\033[96m";
		case Color::BRIGHT_WHITE:
			return "\033[97m";
		case Color::DARK_GRAY:
			return "\033[38;5;240m";
		}
		std::unreachable();
	}
}
