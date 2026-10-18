#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// Text is UTF-8, but a path made from a char string reads it in the narrow
// encoding, which on Windows is the code page. Text becomes a path and a path
// becomes text through UTF-8 here instead.
namespace TextPath
{
	[[nodiscard]] std::filesystem::path FromText(std::string_view text);

	[[nodiscard]] std::string ToText(const std::filesystem::path& path);
}
