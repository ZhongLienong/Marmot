#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace MidoriChecksum
{

	[[nodiscard]] std::expected<std::string, std::string> HashFile(const std::filesystem::path& path);

	[[nodiscard]] std::expected<bool, std::string> VerifyFileChecksum(const std::filesystem::path& path, std::string_view expected_checksum);
}
