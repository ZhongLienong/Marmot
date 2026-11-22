#pragma once

#include <expected>
#include <filesystem>
#include <iosfwd>
#include <string>

#include "Bytecode/Executable/Executable.h"

namespace VmBinaryArtifact
{
	[[nodiscard]] std::expected<VmExecutable, std::string> ReadExecutable(std::istream& in);

	[[nodiscard]] std::expected<VmExecutable, std::string> ReadExecutableFromFile(
		const std::filesystem::path& path);
}
