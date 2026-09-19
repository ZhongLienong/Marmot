#pragma once

#include "Common/Executable/Executable.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Everything the compiler learns from outside the source it is given: where
// `<Name>` imports are found, and how the native libraries the source names
// (`foreign ... from "library"`) may be used. The compiler reads no environment
// variables and no manifests; its caller (a build plan, or MARMOT_PATH for a
// file compiled on its own) resolves these.
class CompilationInputs
{
private:
	std::vector<std::filesystem::path> m_search_paths;
	std::unordered_map<std::string, NativeLibraryPolicy> m_native_library_policies;

public:
	// Directories searched, in order, for `<Name>` imports. Directories that do
	// not exist are dropped and the rest made canonical, once, here.
	CompilationInputs WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&;

	const std::vector<std::filesystem::path>& SearchPaths() const;

	// Recorded in the program for each library it names; a library without one
	// is not thread-safe and has no checksum.
	CompilationInputs WithNativeLibraryPolicies(std::unordered_map<std::string, NativeLibraryPolicy> policies) &&;

	const std::unordered_map<std::string, NativeLibraryPolicy>& NativeLibraryPolicies() const;
};
