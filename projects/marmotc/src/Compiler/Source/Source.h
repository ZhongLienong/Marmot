#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace MarmotSource
{
	struct Position
	{
		int m_line;
		size_t m_column;
	};

	// Editors on Windows write this at the start of a UTF-8 file. It is not part
	// of the program.
	inline constexpr std::string_view UTF8_BYTE_ORDER_MARK = "\xEF\xBB\xBF";

	[[nodiscard]] bool StartsWithByteOrderMark(std::string_view text);

	void RemoveByteOrderMark(std::string& text);
}
