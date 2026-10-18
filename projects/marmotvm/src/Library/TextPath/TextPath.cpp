#include "Library/TextPath/TextPath.h"

namespace TextPath
{
	std::filesystem::path FromText(std::string_view text)
	{
		return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}

	std::string ToText(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}
}
