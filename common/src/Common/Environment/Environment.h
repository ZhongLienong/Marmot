#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace MidoriEnvironment
{
	// The variable's value, or nothing when it is unset or empty.
	[[nodiscard]] std::optional<std::string> Read(const char* name);

	// A PATH-style list: `;` separated on Windows, `:` elsewhere. Empty entries
	// are dropped.
	[[nodiscard]] std::vector<std::filesystem::path> SplitPathList(const std::string& value);

	// The directories in the PATH-style variable `name`, in order.
	[[nodiscard]] std::vector<std::filesystem::path> ReadPathList(const char* name);
}
