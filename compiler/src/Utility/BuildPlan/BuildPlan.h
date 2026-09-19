#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// A build plan names everything a compile needs, so the compiler does no
// discovery of its own: no manifests, no lockfile, no MARMOT_PATH. The marmot
// tool resolves a project and writes one; `marmotc run|check|build|test
// --plan FILE` reads it.
//
//   {
//     "version": 1,
//     "entry": "src/Main.mmt",
//     "search_paths": ["src", "packages/Image", "MarmotPrelude"],
//     "native_packages": [
//       {
//         "name": "Image",
//         "root": "packages/Image",
//         "library": "packages/Image/lib/windows/x64/marmot_image.dll",
//         "functions": { "MIDORI_FFI_Image_ReadInfo": "marmot_image_read_info" },
//         "thread_safe": true,
//         "checksum": "sha256:..."
//       }
//     ]
//   }
//
// Relative paths are relative to the plan file's directory. "version" is
// required; "entry" is required to run, check or build, and absent for `test`,
// which compiles every test file with the plan's inputs. "thread_safe"
// defaults to false and "checksum" is optional. Unknown members are errors, so
// a misspelt input is reported rather than silently left out.
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
