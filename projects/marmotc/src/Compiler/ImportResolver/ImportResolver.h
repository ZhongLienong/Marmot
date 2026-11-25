#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "Support/Optional.h"

class ImportResolver
{
private:
	std::filesystem::path m_current_file_dir;
	std::vector<std::filesystem::path> m_system_search_paths;

public:
	ImportResolver(const std::string& current_file_path, std::vector<std::filesystem::path> system_search_paths);

	// The absolute path an import names: a path relative to the importing file,
	// or a `<Module>` found in the search paths.
	MarmotCompiler::Optional<std::string> Resolve(const std::string& import_specifier) const;

private:
	static std::filesystem::path ResolveCurrentFileDir(const std::filesystem::path& current_file_path);

	static bool IsSystemImport(const std::string& import_specifier);

	MarmotCompiler::Optional<std::string> ResolvePathImport(const std::string& import_specifier) const;

	MarmotCompiler::Optional<std::string> ResolveSystemImport(const std::string& import_specifier) const;

	MarmotCompiler::Optional<std::filesystem::path> FindSystemModulePath(const std::vector<std::filesystem::path>& module_paths) const;

	static std::vector<std::filesystem::path> GetModuleFilePaths(const std::string& module_name);
};
