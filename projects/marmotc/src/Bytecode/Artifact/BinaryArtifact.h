#pragma once

#include <filesystem>
#include <iosfwd>
#include <string>

#include "Bytecode/Executable/Executable.h"
#include "Support/Expected.h"

namespace MarmotBinaryArtifact
{
	[[nodiscard]] MarmotCompiler::Expected<void, std::string> WriteExecutable(
		const MarmotExecutable& executable,
		std::ostream& out,
		bool embed_sources = false);

	[[nodiscard]] MarmotCompiler::Expected<void, std::string> WriteExecutableToFile(
		const MarmotExecutable& executable,
		const std::filesystem::path& path,
		bool embed_sources = false);

}
