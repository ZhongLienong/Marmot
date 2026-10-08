#pragma once

#include <string_view>

namespace CompilerTerminal
{
	enum class Color
	{
		RESET,
		BOLD,
		RED,
		YELLOW,
		BLUE,
		WHITE,
		BRIGHT_RED,
		BRIGHT_YELLOW,
		BRIGHT_CYAN,
	};

	// Whether diagnostics carry colour: off until the driver, which knows where
	// they are going, turns it on before compiling.
	void SetColorEnabled(bool enabled);
	[[nodiscard]] bool ColorEnabled();

	// The escape code for `color`, or nothing when colour is off.
	[[nodiscard]] std::string_view Code(Color color);
}
