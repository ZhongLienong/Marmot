#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// A package that ships a native library. A source file belongs to the package
// when the package root is the file's directory; only such files may call the
// library's functions, and importing one is what makes the program need the
// library.
struct NativePackage
{
	std::string m_name;
	std::filesystem::path m_root;
	std::filesystem::path m_library;
	std::unordered_map<std::string, std::string> m_functions;  // name in Marmot -> symbol in the library
	bool m_thread_safe = false;
	std::optional<std::string> m_checksum = std::nullopt;
};

// Everything the compiler learns from outside the source it is given. The
// compiler reads no environment variables and no manifests: whoever calls it
// (the driver's project discovery today, a build plan from the marmot tool
// later) resolves those and passes the result here.
class CompilationInputs
{
public:
	using NativePackageLookup = std::function<std::optional<NativePackage>(const std::filesystem::path& directory)>;

private:
	std::vector<std::filesystem::path> m_search_paths;
	NativePackageLookup m_find_native_package;

public:
	// Directories searched, in order, for `<Name>` imports. Directories that do
	// not exist are dropped and the rest made canonical, once, here.
	CompilationInputs WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&;

	// Native packages decided up front (a build plan).
	CompilationInputs WithNativePackages(std::vector<NativePackage> native_packages) &&;

	// Native packages found on demand (the driver reading package manifests).
	CompilationInputs WithNativePackageLookup(NativePackageLookup find_native_package) &&;

	const std::vector<std::filesystem::path>& SearchPaths() const;

	// The native package whose root is `directory`, if there is one.
	std::optional<NativePackage> FindNativePackage(const std::filesystem::path& directory) const;
};
