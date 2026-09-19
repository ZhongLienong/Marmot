#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A build plan names everything a compile needs, so the compiler does no
// discovery of its own: no manifests, no lockfile, no MARMOT_PATH. The marmot
// tool resolves a project and writes one; `marmotc run|check|build|test
// --plan FILE` reads it.
//
//   {
//     "version": 1,
//     "entry": "src/Main.mmt",
//     "search_paths": ["src", "packages/Image-1.0.0", "MarmotPrelude"],
//     "native_libraries": [
//       { "name": "marmot_image", "checksum": "sha256:...", "thread_safe": true }
//     ]
//   }
//
// Relative paths are relative to the plan file's directory. "version" is
// required; "entry" is required to run, check or build, and absent for `test`,
// which compiles every test file with the plan's inputs. A native library's
// checksum and thread_safe flag are recorded in the program; where its file is
// belongs to the machine that runs it, so a plan has no library paths. Unknown
// members are errors, so a misspelt input is reported rather than silently left
// out.
struct BuildPlan
{
	static constexpr int VERSION = 1;

	std::optional<std::filesystem::path> m_entry = std::nullopt;
	CompilationInputs m_inputs;
};

namespace MidoriBuildPlan
{
	// `base_directory` resolves the plan's relative paths.
	[[nodiscard]] std::expected<BuildPlan, std::string> Parse(std::string_view json, const std::filesystem::path& base_directory);

	[[nodiscard]] std::expected<BuildPlan, std::string> ReadFile(const std::filesystem::path& plan_path);
}
