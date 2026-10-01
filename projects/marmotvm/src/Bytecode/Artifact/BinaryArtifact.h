#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "Bytecode/Executable/Executable.h"

namespace VmBinaryArtifact
{
	static constexpr uint8_t s_magic[4] = { 'M', 'B', 'C', '\0' };

	[[nodiscard]] std::expected<VmExecutable, std::string> ReadExecutable(std::istream& in);

	[[nodiscard]] std::expected<VmExecutable, std::string> ReadExecutableFromFile(
		const std::filesystem::path& path);
}
