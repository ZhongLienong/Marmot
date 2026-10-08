#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ImportResolver
{
private:
	std::filesystem::path m_current_file_dir;
	std::vector<std::filesystem::path> m_system_search_paths;

public:
	ImportResolver(const std::string& current_file_path, std::vector<std::filesystem::path> system_search_paths);

	// The absolute path an import names: a path relative to the importing file,
	// or a `<Module>` found in the search paths.
	std::optional<std::string> Resolve(const std::string& import_specifier) const;

private:
	static std::filesystem::path ResolveCurrentFileDir(const std::filesystem::path& current_file_path);

	static bool IsSystemImport(const std::string& import_specifier);

	std::optional<std::string> ResolvePathImport(const std::string& import_specifier) const;

	std::optional<std::string> ResolveSystemImport(const std::string& import_specifier) const;

	std::optional<std::filesystem::path> FindSystemModulePath(const std::vector<std::filesystem::path>& module_paths) const;

	static std::vector<std::filesystem::path> GetModuleFilePaths(const std::string& module_name);
};
