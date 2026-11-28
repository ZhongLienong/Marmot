#include "ImportResolver.h"
#include "Support/Optional.h"
#include <algorithm>
#include <utility>

using namespace std::string_literals;

ImportResolver::ImportResolver(const std::string& current_file_path, std::vector<std::filesystem::path> system_search_paths)
	: m_current_file_dir(ResolveCurrentFileDir(std::filesystem::path(current_file_path))),
	m_system_search_paths(std::move(system_search_paths))
{
}

std::filesystem::path ImportResolver::ResolveCurrentFileDir(const std::filesystem::path& current_file_path)
{
	if (current_file_path.has_parent_path())
	{
		return current_file_path.parent_path();
	}

	return std::filesystem::current_path();
}

MarmotCompiler::Optional<std::string> ImportResolver::ResolvePathImport(const std::string& import_specifier) const
{
	std::filesystem::path resolved_path(import_specifier);

#ifdef __EMSCRIPTEN__
	if (!resolved_path.is_absolute())
	{
		resolved_path = m_current_file_dir / resolved_path;
	}
	std::string absolute_path = resolved_path.lexically_normal().string();
#else
	if (!resolved_path.is_absolute())
	{
		resolved_path = m_current_file_dir / resolved_path;
	}

	if (!std::filesystem::exists(resolved_path))
	{
		return MarmotCompiler::None;
	}

	std::string absolute_path = std::filesystem::weakly_canonical(resolved_path).string();
#endif

	return absolute_path;
}

MarmotCompiler::Optional<std::filesystem::path> ImportResolver::FindSystemModulePath(const std::vector<std::filesystem::path>& module_paths) const
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

	return MarmotCompiler::None;
}

MarmotCompiler::Optional<std::string> ImportResolver::ResolveSystemImport(const std::string& module_name) const
{
	std::vector<std::filesystem::path> possible_paths = GetModuleFilePaths(module_name);
	MarmotCompiler::Optional<std::filesystem::path> resolved_path = FindSystemModulePath(possible_paths);

	if (!resolved_path.HasValue())
	{
		return MarmotCompiler::None;
	}

	return resolved_path->string();
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
