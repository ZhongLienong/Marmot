#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "Common/Executable/Executable.h"

namespace MidoriBytecodeArtifact
{
	struct ArtifactResult
	{
		std::filesystem::path m_path;
		std::string m_json;
	};

	[[nodiscard]] std::expected<ArtifactResult, std::string> WriteExecutableArtifact(
		const MidoriExecutable& executable,
		const std::filesystem::path& source_file);
}
