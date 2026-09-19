#include "CompilationInputs.h"

#include <utility>

CompilationInputs CompilationInputs::WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&
{
	m_search_paths.clear();
	for (const std::filesystem::path& path : search_paths)
	{
		std::error_code error;
		if (!std::filesystem::is_directory(path, error))
		{
			continue;
		}

		std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
		if (error)
		{
			continue;
		}

		m_search_paths.emplace_back(std::move(canonical));
	}

	return std::move(*this);
}

const std::vector<std::filesystem::path>& CompilationInputs::SearchPaths() const
{
	return m_search_paths;
}

CompilationInputs CompilationInputs::WithNativeLibraryPolicies(std::unordered_map<std::string, NativeLibraryPolicy> policies) &&
{
	m_native_library_policies = std::move(policies);
	return std::move(*this);
}

const std::unordered_map<std::string, NativeLibraryPolicy>& CompilationInputs::NativeLibraryPolicies() const
{
	return m_native_library_policies;
}
