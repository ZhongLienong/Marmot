#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// How a native library the program names (`foreign ... from "library"`) is
// found and loaded when it runs.
struct NativeLibrarySettings
{
	// Tried before any search path.
	std::optional<std::filesystem::path> m_path = std::nullopt;
	// Verified before the library loads.
	std::optional<std::string> m_checksum = std::nullopt;
	// Whether workers may call it concurrently.
	bool m_thread_safe = false;
};

struct NativeLibraryOptions
{
	// Directories holding `name.dll`, `libname.so` or `libname.dylib`, in order.
	std::vector<std::filesystem::path> m_search_paths;
	std::unordered_map<std::string, NativeLibrarySettings> m_libraries;
};

// A build plan names everything a compile and run needs, so the compiler does
// no discovery of its own: no manifests, no lockfile, no MARMOT_PATH. The marmot
// tool resolves a project and writes one; `marmotc run|check|build|test
// --plan FILE` reads it.
//
//   {
//     "version": 1,
//     "entry": "src/Main.mmt",
//     "search_paths": ["src", "packages/Image-1.0.0", "MarmotPrelude"],
//     "library_paths": ["native/bin"],
//     "native_libraries": [
//       {
//         "name": "marmot_image",
//         "path": "packages/Image-1.0.0/lib/windows/x64/marmot_image.dll",
//         "checksum": "sha256:...",
//         "thread_safe": true
//       }
//     ]
//   }
//
// Relative paths are relative to the plan file's directory. "version" is
// required; "entry" is required to run, check or build, and absent for `test`,
// which compiles every test file with the plan's inputs. A native library
// needs only its "name". Unknown members are errors, so a misspelt input is
// reported rather than silently left out.
struct BuildPlan
{
	static constexpr int VERSION = 1;

	std::optional<std::filesystem::path> m_entry = std::nullopt;
	CompilationInputs m_inputs;
	NativeLibraryOptions m_native;
};

namespace MidoriBuildPlan
{
	// `base_directory` resolves the plan's relative paths.
	[[nodiscard]] std::expected<BuildPlan, std::string> Parse(std::string_view json, const std::filesystem::path& base_directory);

	[[nodiscard]] std::expected<BuildPlan, std::string> ReadFile(const std::filesystem::path& plan_path);
}
