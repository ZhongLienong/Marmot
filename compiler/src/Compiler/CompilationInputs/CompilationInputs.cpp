#include "CompilationInputs.h"

#include <memory>
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

CompilationInputs CompilationInputs::WithNativePackages(std::vector<NativePackage> native_packages) &&
{
	std::shared_ptr<const std::vector<NativePackage>> packages = std::make_shared<const std::vector<NativePackage>>(std::move(native_packages));
	m_find_native_package = [packages](const std::filesystem::path& directory) -> std::optional<NativePackage>
	{
		std::error_code error;
		for (const NativePackage& package : *packages)
		{
			if (std::filesystem::equivalent(package.m_root, directory, error))
			{
				return package;
			}
		}

		return std::nullopt;
	};

	return std::move(*this);
}

CompilationInputs CompilationInputs::WithNativePackageLookup(NativePackageLookup find_native_package) &&
{
	m_find_native_package = std::move(find_native_package);
	return std::move(*this);
}

const std::vector<std::filesystem::path>& CompilationInputs::SearchPaths() const
{
	return m_search_paths;
}

std::optional<NativePackage> CompilationInputs::FindNativePackage(const std::filesystem::path& directory) const
{
	if (!m_find_native_package)
	{
		return std::nullopt;
	}

	return m_find_native_package(directory);
}
