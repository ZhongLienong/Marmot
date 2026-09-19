#include "Common/Environment/Environment.h"

#include <cstdlib>
#include <ranges>

namespace MidoriEnvironment
{
	std::optional<std::string> Read(const char* name)
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
		{
			return std::nullopt;
		}

		std::string result(value);
		free(value);
		return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
#else
		const char* value = std::getenv(name);
		return value == nullptr || value[0] == '\0' ? std::nullopt : std::optional<std::string>(value);
#endif
	}

	std::vector<std::filesystem::path> SplitPathList(const std::string& value)
	{
#ifdef _WIN32
		const char separator = ';';
#else
		const char separator = ':';
#endif
		std::vector<std::filesystem::path> paths;
		for (const std::ranges::subrange<std::string::const_iterator> segment : value | std::views::split(separator))
		{
			if (!segment.empty())
			{
				paths.emplace_back(std::string(segment.begin(), segment.end()));
			}
		}
		return paths;
	}

	std::vector<std::filesystem::path> ReadPathList(const char* name)
	{
		const std::optional<std::string> value = Read(name);
		return value.has_value() ? SplitPathList(value.value()) : std::vector<std::filesystem::path>{};
	}
}
