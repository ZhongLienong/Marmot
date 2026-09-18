#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "Common/Executable/Executable.h"

namespace MidoriBinaryArtifact
{
	static constexpr uint8_t s_magic[4] = { 'M', 'B', 'C', '\0' };

	[[nodiscard]] std::expected<void, std::string> WriteExecutable(
		const MidoriExecutable& executable,
		std::ostream& out,
		bool embed_sources = false);

	[[nodiscard]] std::expected<MidoriExecutable, std::string> ReadExecutable(std::istream& in);

	[[nodiscard]] std::expected<void, std::string> WriteExecutableToFile(
		const MidoriExecutable& executable,
		const std::filesystem::path& path,
		bool embed_sources = false);

	[[nodiscard]] std::expected<MidoriExecutable, std::string> ReadExecutableFromFile(
		const std::filesystem::path& path);
}
