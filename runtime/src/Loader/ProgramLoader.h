#pragma once

#include "Common/Error/Error.h"
#include "Common/Executable/Executable.h"

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Everything between a compiled program and its run: reading a .mmc, loading
// the native libraries it names, and running it. marmotvm is a thin command
// line over this; nothing here compiles.
namespace MidoriProgramLoader
{
	// Where the native libraries a program names are, on this machine.
	struct NativeLibraryLocations
	{
		// A library's file, by the name source gives it; tried first.
		std::unordered_map<std::string, std::filesystem::path> m_files;
		// Directories holding `name.dll`, `libname.so` or `libname.dylib`, in order.
		std::vector<std::filesystem::path> m_search_paths;
	};

	// The directories in MARMOT_LIBRARY_PATH, in order.
	[[nodiscard]] std::vector<std::filesystem::path> EnvironmentLibraryPaths();

	[[nodiscard]] std::expected<MidoriExecutable, std::string> ReadProgram(const std::filesystem::path& path);

	// Loads every native library the program names (`foreign ... from
	// "library"`), before it runs. Each is looked for at its file in
	// `locations`, then in each search path, then beside the modules that
	// declared it (in lib/<platform>/, then the directory itself). The
	// library's thread_safe flag and checksum come from the program. A library
	// that is missing, fails to load, or lacks a symbol is an error. Loading
	// runs a library's own code.
	[[nodiscard]] std::expected<void, std::string> LoadNativeLibraries(const MidoriExecutable& executable, const NativeLibraryLocations& locations);

	[[nodiscard]] std::expected<int, RuntimeError> Run(MidoriExecutable&& executable);
}
