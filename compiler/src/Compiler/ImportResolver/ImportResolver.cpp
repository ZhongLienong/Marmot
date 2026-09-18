#include "ImportResolver.h"
#include <algorithm>
#include <cstdlib>
#include <utility>

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace std::string_literals;

ImportResolver::ImportResolver(const std::string& current_file_path)
	: ImportResolver(ResolveCurrentFileDir(std::filesystem::path(current_file_path)),
		CollectSystemSearchPaths())
{
}

ImportResolver::ImportResolver(std::filesystem::path current_file_dir, std::vector<std::filesystem::path> system_search_paths)
	: m_current_file_dir(std::move(current_file_dir)),
		m_system_search_paths(std::move(system_search_paths))
{
}

const std::vector<std::filesystem::path>& ImportResolver::GetSystemSearchPaths() const
{
	return m_system_search_paths;
}

ImportResolver ImportResolver::WithSystemSearchPaths(std::vector<std::filesystem::path> system_search_paths) const &
{
	return ImportResolver(m_current_file_dir, std::move(system_search_paths));
}

ImportResolver ImportResolver::WithSystemSearchPaths(std::vector<std::filesystem::path> system_search_paths) &&
{
	return ImportResolver(std::move(m_current_file_dir), std::move(system_search_paths));
}

std::filesystem::path ImportResolver::ResolveCurrentFileDir(const std::filesystem::path& current_file_path)
{
	if (current_file_path.has_parent_path())
	{
		return current_file_path.parent_path();
	}

	return std::filesystem::current_path();
}

std::vector<std::filesystem::path> ImportResolver::CollectSystemSearchPaths()
{
	std::optional<std::string> env_value = ReadEnvironmentVariable("MARMOT_PATH");
	if (!env_value.has_value())
	{
		return {};
	}

#ifdef _WIN32
	const char separator = ';';
#else
	const char separator = ':';
#endif

	std::vector<std::filesystem::path> split_paths = SplitSearchPaths(env_value.value(), separator);
	return CanonicalizeDirectories(split_paths);
}

std::optional<std::string> ImportResolver::ReadEnvironmentVariable(const char* name)
{
#ifdef _WIN32
	char* value = nullptr;
	size_t len = 0u;
	if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
	{
		return std::nullopt;
	}

	std::string result(value);
	std::free(value);
	if (result.empty())
	{
		return std::nullopt;
	}

	return result;
#else
	const char* value = std::getenv(name);
	if (value == nullptr || value[0] == '\0')
	{
		return std::nullopt;
	}

	return std::string(value);
#endif
}

std::vector<std::filesystem::path> ImportResolver::SplitSearchPaths(const std::string& path_str, char separator)
{
	std::vector<std::filesystem::path> paths;
	size_t start = 0u;
	size_t end = path_str.find(separator);

	while (end != std::string::npos)
	{
		std::string path_segment = path_str.substr(start, end - start);
		if (!path_segment.empty())
		{
			paths.emplace_back(path_segment);
		}
		start = end + 1u;
		end = path_str.find(separator, start);
	}

	std::string last_segment = path_str.substr(start);
	if (!last_segment.empty())
	{
		paths.emplace_back(last_segment);
	}

	return paths;
}

std::vector<std::filesystem::path> ImportResolver::CanonicalizeDirectories(const std::vector<std::filesystem::path>& paths)
{
	std::vector<std::filesystem::path> directories;
	for (const std::filesystem::path& path : paths)
	{
		if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
		{
			directories.push_back(std::filesystem::weakly_canonical(path));
		}
	}

	return directories;
}

std::optional<ImportResolver::ResolvedImport> ImportResolver::Resolve(const std::string& import_specifier) const
{
	return IsSystemImport(import_specifier)
		? ResolveSystemImport(import_specifier)
		: ResolvePathImport(import_specifier);
}

bool ImportResolver::IsSystemImport(const std::string& import_specifier)
{
	return import_specifier.size() >= 3
		&& import_specifier.front() == '<'
		&& import_specifier.back() == '>';
}

std::string ImportResolver::ExtractModuleName(const std::string& system_import)
{
	if (system_import.size() >= 2 && system_import.front() == '<' && system_import.back() == '>')
	{
		return system_import.substr(1, system_import.size() - 2);
	}
	return system_import;
}

ImportResolver::ResolvedImport ImportResolver::MakeResolvedImport(std::string absolute_path, ImportType type, std::string original_specifier)
{
	ResolvedImport result;
	result.m_absolute_path = std::move(absolute_path);
	result.m_original_specifier = std::move(original_specifier);
	result.m_type = type;
	return result;
}

std::optional<ImportResolver::ResolvedImport> ImportResolver::ResolvePathImport(const std::string& import_specifier) const
{
	std::filesystem::path resolved_path(import_specifier);

#ifdef __EMSCRIPTEN__
	if (!resolved_path.is_absolute())
	{
		resolved_path = m_current_file_dir / resolved_path;
	}
	std::string absolute_path = resolved_path.string();
#else
	if (!resolved_path.is_absolute())
	{
		resolved_path = m_current_file_dir / resolved_path;
	}

	if (!std::filesystem::exists(resolved_path))
	{
		return std::nullopt;
	}

	std::string absolute_path = std::filesystem::weakly_canonical(resolved_path).string();
#endif

	return MakeResolvedImport(std::move(absolute_path), ImportType::PATH, import_specifier);
}

std::optional<std::filesystem::path> ImportResolver::FindSystemModulePath(const std::vector<std::filesystem::path>& module_paths) const
{
	for (const std::filesystem::path& search_dir : m_system_search_paths)
	{
		for (const std::filesystem::path& module_path : module_paths)
		{
			std::filesystem::path full_path = search_dir / module_path;
			if (std::filesystem::exists(full_path) && std::filesystem::is_regular_file(full_path))
			{
				return std::filesystem::weakly_canonical(full_path);
			}
		}
	}

	return std::nullopt;
}

std::optional<ImportResolver::ResolvedImport> ImportResolver::ResolveSystemImport(const std::string& import_specifier) const
{
	std::string module_name = ExtractModuleName(import_specifier);
	std::vector<std::filesystem::path> possible_paths = GetModuleFilePaths(module_name);
	std::optional<std::filesystem::path> resolved_path = FindSystemModulePath(possible_paths);

	if (!resolved_path.has_value())
	{
		return std::nullopt;
	}

	return MakeResolvedImport(resolved_path->string(), ImportType::SYSTEM, import_specifier);
}

std::vector<std::filesystem::path> ImportResolver::GetModuleFilePaths(const std::string& module_name)
{
	std::vector<std::filesystem::path> paths;

	std::string path_str = module_name;
	std::ranges::replace(path_str, '.', static_cast<char>(std::filesystem::path::preferred_separator));
	paths.push_back(path_str + ".mmt"s);

	if (module_name.find('.') == std::string::npos)
	{
		paths.push_back(module_name + ".mmt"s);
	}

	return paths;
}
