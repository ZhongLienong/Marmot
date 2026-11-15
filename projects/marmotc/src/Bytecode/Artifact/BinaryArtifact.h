#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "Bytecode/Executable/Executable.h"

namespace MarmotBinaryArtifact
{
	static constexpr uint8_t s_magic[4] = { 'M', 'B', 'C', '\0' };

	[[nodiscard]] std::expected<void, std::string> WriteExecutable(
		const MarmotExecutable& executable,
		std::ostream& out,
		bool embed_sources = false);

	[[nodiscard]] std::expected<void, std::string> WriteExecutableToFile(
		const MarmotExecutable& executable,
		const std::filesystem::path& path,
		bool embed_sources = false);

}
