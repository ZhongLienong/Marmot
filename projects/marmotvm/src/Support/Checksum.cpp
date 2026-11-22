#include "Checksum.h"

#include <array>
#include <format>
#include <fstream>

#include <picosha2.h>

namespace MarmotChecksum
{

	std::expected<std::string, std::string> HashFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return std::unexpected(std::format("Failed to open file for hashing: {}", path.string()));
		}

		picosha2::hash256_one_by_one hash;
		std::array<char, 4096u> buffer{};
		while (input.good())
		{
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize count = input.gcount();
			if (count > 0)
			{
				hash.process(buffer.begin(), buffer.begin() + count);
			}
		}

		if (!input.eof())
		{
			return std::unexpected(std::format("Failed while reading file for hashing: {}", path.string()));
		}

		hash.finish();
		return "sha256:" + picosha2::get_hash_hex_string(hash);
	}

	std::expected<bool, std::string> VerifyFileChecksum(const std::filesystem::path& path, std::string_view expected_checksum)
	{
		if (!expected_checksum.starts_with("sha256:"))
		{
			return std::unexpected(std::format("Unsupported checksum format '{}'.", expected_checksum));
		}

		const std::expected<std::string, std::string> actual_checksum = HashFile(path);
		if (!actual_checksum.has_value())
		{
			return std::unexpected(actual_checksum.error());
		}

		return actual_checksum.value() == expected_checksum;
	}
}
