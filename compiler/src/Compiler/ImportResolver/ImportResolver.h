#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

class ImportResolver
{
public:
	enum class ImportType
	{
		PATH,    // Relative or absolute file path
		SYSTEM   // System module (uses MARMOT_PATH)
	};

	struct ResolvedImport
	{
		std::string m_absolute_path;
		std::string m_original_specifier;  // Original import string for error messages
		ImportType m_type;
	};

private:
	std::filesystem::path m_current_file_dir;
	std::vector<std::filesystem::path> m_system_search_paths;

public:
	explicit ImportResolver(const std::string& current_file_path);

	std::optional<ResolvedImport> Resolve(const std::string& import_specifier) const;

	const std::vector<std::filesystem::path>& GetSystemSearchPaths() const;

	ImportResolver WithSystemSearchPaths(std::vector<std::filesystem::path> system_search_paths) const &;

	ImportResolver WithSystemSearchPaths(std::vector<std::filesystem::path> system_search_paths) &&;

private:
	ImportResolver(std::filesystem::path current_file_dir, std::vector<std::filesystem::path> system_search_paths);

	static std::filesystem::path ResolveCurrentFileDir(const std::filesystem::path& current_file_path);

	static std::vector<std::filesystem::path> CollectSystemSearchPaths();

	static std::optional<std::string> ReadEnvironmentVariable(const char* name);

	static std::vector<std::filesystem::path> SplitSearchPaths(const std::string& path_str, char separator);

	static std::vector<std::filesystem::path> CanonicalizeDirectories(const std::vector<std::filesystem::path>& paths);

	static bool IsSystemImport(const std::string& import_specifier);

	static std::string ExtractModuleName(const std::string& system_import);

	std::optional<ResolvedImport> ResolvePathImport(const std::string& import_specifier) const;

	std::optional<ResolvedImport> ResolveSystemImport(const std::string& import_specifier) const;

	std::optional<std::filesystem::path> FindSystemModulePath(const std::vector<std::filesystem::path>& module_paths) const;

	static std::vector<std::filesystem::path> GetModuleFilePaths(const std::string& module_name);

	static ResolvedImport MakeResolvedImport(std::string absolute_path, ImportType type, std::string original_specifier);
};
