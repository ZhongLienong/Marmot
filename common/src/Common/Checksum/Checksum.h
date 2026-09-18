#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MidoriChecksum
{
	[[nodiscard]] std::string HashBytes(std::string_view bytes);

	[[nodiscard]] std::expected<std::string, std::string> HashFile(const std::filesystem::path& path);

	[[nodiscard]] std::expected<std::string, std::string> HashFiles(const std::vector<std::filesystem::path>& files, const std::filesystem::path& root);

	[[nodiscard]] std::expected<std::string, std::string> HashPackageSources(const std::filesystem::path& package_directory);

	[[nodiscard]] std::expected<bool, std::string> VerifyFileChecksum(const std::filesystem::path& path, std::string_view expected_checksum);
}
