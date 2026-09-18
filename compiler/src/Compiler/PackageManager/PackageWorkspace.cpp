#include "Compiler/PackageManager/PackageWorkspace.h"

#include "Common/Checksum/Checksum.h"
#include "Compiler/PackageManager/Lockfile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <format>
#include <ranges>
#include <system_error>
#include <unordered_set>

namespace
{
	[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(const char* name)
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
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

	[[nodiscard]] std::vector<std::filesystem::path> SplitSearchPaths(const std::string& path_str)
	{
		std::vector<std::filesystem::path> paths;
#ifdef _WIN32
		const char separator = ';';
#else
		const char separator = ':';
#endif

		size_t start = 0u;
		while (start <= path_str.size())
		{
			const size_t end = path_str.find(separator, start);
			const std::string segment = path_str.substr(start, end == std::string::npos ? path_str.size() - start : end - start);
			if (!segment.empty())
			{
				paths.emplace_back(segment);
			}

			if (end == std::string::npos)
			{
				break;
			}
			start = end + 1u;
		}

		return paths;
	}

	[[nodiscard]] std::filesystem::path NormalizePath(const std::filesystem::path& path)
	{
		std::error_code error_code;
		std::filesystem::path absolute_path = path;
		if (!absolute_path.is_absolute())
		{
			absolute_path = std::filesystem::absolute(path, error_code);
			if (error_code)
			{
				absolute_path = path;
			}
		}

		std::filesystem::path canonical_path = std::filesystem::weakly_canonical(absolute_path, error_code);
		if (error_code)
		{
			return absolute_path.lexically_normal();
		}
		return canonical_path;
	}

	[[nodiscard]] std::string NormalizePathKey(const std::filesystem::path& path)
	{
		std::string normalized = NormalizePath(path).generic_string();
#ifdef _WIN32
		std::ranges::transform(
			normalized,
			normalized.begin(),
			[](unsigned char value)
			{
				return static_cast<char>(std::tolower(value));
			});
#endif
		return normalized;
	}

	void AppendUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate)
	{
		if (candidate.empty())
		{
			return;
		}

		std::error_code error_code;
		if (!std::filesystem::exists(candidate, error_code) || !std::filesystem::is_directory(candidate, error_code))
		{
			return;
		}

		const std::string candidate_key = NormalizePathKey(candidate);
		for (const std::filesystem::path& existing : paths)
		{
			if (NormalizePathKey(existing) == candidate_key)
			{
				return;
			}
		}

		paths.push_back(candidate);
	}

	[[nodiscard]] std::filesystem::path ResolveConfiguredDirectory(
		const MidoriProject::ManifestConfiguration& configuration,
		const std::filesystem::path& configured_path,
		std::string_view fallback_directory)
	{
		if (!configured_path.empty())
		{
			return configured_path.is_absolute() ? configured_path : configuration.m_root / configured_path;
		}
		return configuration.m_root / std::filesystem::path(fallback_directory);
	}

	[[nodiscard]] std::vector<std::filesystem::path> CollectEnvironmentPaths()
	{
		const std::optional<std::string> env_value = ReadEnvironmentVariable("MARMOT_PATH");
		if (!env_value.has_value())
		{
			return {};
		}

		std::vector<std::filesystem::path> paths;
		for (const std::filesystem::path& path : SplitSearchPaths(*env_value))
		{
			AppendUniquePath(paths, path);
		}
		return paths;
	}

	[[nodiscard]] std::vector<std::filesystem::path> BuildSearchPaths(
		const MidoriProject::ManifestConfiguration& configuration,
		const MidoriPackageManager::ResolvedPackageGraph& graph,
		const std::vector<std::filesystem::path>& env_paths)
	{
		std::vector<std::filesystem::path> paths;
		AppendUniquePath(paths, ResolveConfiguredDirectory(configuration, configuration.m_source_dir, "src"));
		for (const std::filesystem::path& package_path : graph.PackageSearchPaths())
		{
			AppendUniquePath(paths, package_path);
		}
		for (const std::filesystem::path& extra_path : configuration.m_extra_paths)
		{
			AppendUniquePath(paths, extra_path.is_absolute() ? extra_path : configuration.m_root / extra_path);
		}
		AppendUniquePath(paths, ResolveConfiguredDirectory(configuration, configuration.m_prelude_dir, "MarmotPrelude"));
		for (const std::filesystem::path& env_path : env_paths)
		{
			AppendUniquePath(paths, env_path);
		}
		return paths;
	}

	[[nodiscard]] std::vector<std::filesystem::path> CollectPackageIndexRoots(
		const MidoriProject::ManifestConfiguration& configuration,
		const std::vector<std::filesystem::path>& env_paths)
	{
		std::vector<std::filesystem::path> roots;
		AppendUniquePath(roots, ResolveConfiguredDirectory(configuration, configuration.m_packages_dir, "packages"));
		AppendUniquePath(roots, MidoriPackageManager::GetGlobalPackageCacheDirectory());
		for (const std::filesystem::path& extra_path : configuration.m_extra_paths)
		{
			AppendUniquePath(roots, extra_path.is_absolute() ? extra_path : configuration.m_root / extra_path);
		}
		for (const std::filesystem::path& env_path : env_paths)
		{
			AppendUniquePath(roots, env_path);
		}
		return roots;
	}

	[[nodiscard]] std::expected<std::string, std::string> GetManifestChecksum(const MidoriProject::ManifestConfiguration& configuration)
	{
		return MidoriChecksum::HashFile(configuration.m_manifest_path);
	}

	[[nodiscard]] std::expected<void, std::string> CopyDirectoryReplacing(
		const std::filesystem::path& source,
		const std::filesystem::path& target)
	{
		std::error_code error_code;
		if (std::filesystem::exists(target, error_code))
		{
			std::filesystem::remove_all(target, error_code);
			if (error_code)
			{
				return std::unexpected(std::format("Failed to clear package directory '{}': {}", target.string(), error_code.message()));
			}
		}

		std::filesystem::create_directories(target.parent_path(), error_code);
		if (error_code)
		{
			return std::unexpected(std::format("Failed to create package directory '{}': {}", target.parent_path().string(), error_code.message()));
		}

		std::filesystem::copy(
			source,
			target,
			std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
			error_code);
		if (error_code)
		{
			return std::unexpected(std::format(
				"Failed to copy package from '{}' to '{}': {}",
				source.string(),
				target.string(),
				error_code.message()));
		}

		return {};
	}

	[[nodiscard]] std::expected<std::string, std::string> ComputePackageChecksum(const MidoriPackageManager::ResolvedPackage& package)
	{
		if (!package.m_package_checksum.empty())
		{
			return package.m_package_checksum;
		}
		return MidoriChecksum::HashPackageSources(package.m_manifest.GetPackageDirectory());
	}
}

namespace MidoriPackageManager
{
	std::filesystem::path GetGlobalPackageCacheDirectory()
	{
#ifdef _WIN32
		const std::optional<std::string> local_app_data = ReadEnvironmentVariable("LOCALAPPDATA");
		if (local_app_data.has_value())
		{
			return std::filesystem::path(*local_app_data) / "Marmot" / "cache";
		}
#endif
		const std::optional<std::string> user_profile = ReadEnvironmentVariable("USERPROFILE");
		if (user_profile.has_value())
		{
			return std::filesystem::path(*user_profile) / ".marmot" / "cache";
		}

		const std::optional<std::string> home = ReadEnvironmentVariable("HOME");
		if (home.has_value())
		{
			return std::filesystem::path(*home) / ".marmot" / "cache";
		}

		return std::filesystem::current_path() / ".marmot" / "cache";
	}

	std::expected<ResolvedPackageGraph, std::string> InstallPackagesLocally(
		const MidoriProject::ManifestConfiguration& configuration,
		const ResolvedPackageGraph& graph)
	{
		const std::filesystem::path packages_directory = ResolveConfiguredDirectory(configuration, configuration.m_packages_dir, "packages");
		std::error_code error_code;
		std::filesystem::create_directories(packages_directory, error_code);
		if (error_code)
		{
			return std::unexpected(std::format(
				"Failed to create packages directory '{}': {}",
				packages_directory.string(),
				error_code.message()));
		}

		ResolvedPackageGraph localized_graph;
		localized_graph.m_root_dependencies = graph.m_root_dependencies;

		for (const std::string& package_name : graph.TopologicalOrder())
		{
			const ResolvedPackage& package = graph.m_packages.at(package_name);
			const std::string version = package.m_manifest.GetInfo().m_version;
			const std::filesystem::path source_directory = package.m_manifest.GetPackageDirectory();
			const std::filesystem::path target_directory = packages_directory / std::filesystem::path(package_name + "-" + version);

			const std::string source_key = NormalizePathKey(source_directory);
			const std::string target_key = NormalizePathKey(target_directory);
			const std::expected<std::string, std::string> source_checksum = ComputePackageChecksum(package);
			if (!source_checksum.has_value())
			{
				return std::unexpected(source_checksum.error());
			}

			if (source_key != target_key)
			{
				bool should_copy = true;
				if (std::filesystem::exists(target_directory, error_code))
				{
					const std::expected<std::string, std::string> target_checksum = MidoriChecksum::HashPackageSources(target_directory);
					if (target_checksum.has_value() && target_checksum.value() == source_checksum.value())
					{
						should_copy = false;
					}
				}

				if (should_copy)
				{
					const std::expected<void, std::string> copy_result = CopyDirectoryReplacing(source_directory, target_directory);
					if (!copy_result.has_value())
					{
						return std::unexpected(copy_result.error());
					}
				}
			}

			const std::expected<PackageManifest, std::string> localized_manifest = PackageManifest::LoadWithError(target_directory);
			if (!localized_manifest.has_value())
			{
				return std::unexpected(localized_manifest.error());
			}

			ResolvedPackage localized_package;
			localized_package.m_manifest = localized_manifest.value();
			localized_package.m_dependencies = package.m_dependencies;
			localized_package.m_package_checksum = source_checksum.value();
			localized_package.m_source = std::format(
				"local:{}",
				std::filesystem::relative(target_directory, configuration.m_root).generic_string());
			localized_graph.m_packages[package_name] = std::move(localized_package);
		}

		return localized_graph;
	}

	std::expected<MidoriVersion::SemanticVersion, std::string> FindLatestAvailableVersion(
		const MidoriProject::ManifestConfiguration& configuration,
		std::string_view package_name)
	{
		const std::vector<std::filesystem::path> env_paths = CollectEnvironmentPaths();
		const std::vector<std::filesystem::path> roots = CollectPackageIndexRoots(configuration, env_paths);
		const std::expected<PackageIndex, std::string> index = PackageIndex::Scan(roots);
		if (!index.has_value())
		{
			return std::unexpected(index.error());
		}

		const std::vector<PackageIndexEntry> packages = index->FindPackages(package_name);
		if (packages.empty())
		{
			return std::unexpected(std::format("Package '{}' was not found in the local package index.", package_name));
		}

		return packages.front().m_manifest.GetInfo().m_semantic_version;
	}

	std::expected<PackageEnvironment, std::string> PreparePackageEnvironment(
		const MidoriProject::ManifestConfiguration& configuration,
		ResolveMode mode)
	{
		PackageEnvironment environment;
		environment.m_lockfile_path = configuration.m_root / "marmot.lock";

		const std::vector<std::filesystem::path> env_paths = CollectEnvironmentPaths();
		const std::expected<std::string, std::string> manifest_checksum = GetManifestChecksum(configuration);
		if (!manifest_checksum.has_value())
		{
			return std::unexpected(manifest_checksum.error());
		}

		if (mode == ResolveMode::PreferLockfile && std::filesystem::exists(environment.m_lockfile_path))
		{
			const std::expected<LockfileLoadResult, std::string> lockfile = ReadLockfile(environment.m_lockfile_path, manifest_checksum.value());
			if (lockfile.has_value())
			{
				environment.m_warnings = lockfile->m_warnings;
				if (lockfile->m_manifest_matches && lockfile->m_packages_available)
				{
					environment.m_graph = lockfile->m_graph;
					environment.m_search_paths = BuildSearchPaths(configuration, environment.m_graph, env_paths);
					environment.m_used_lockfile = true;
					return environment;
				}
			}
		}

		const std::vector<std::filesystem::path> roots = CollectPackageIndexRoots(configuration, env_paths);
		const std::expected<PackageIndex, std::string> index = PackageIndex::Scan(roots);
		if (!index.has_value())
		{
			return std::unexpected(index.error());
		}

		const PackageResolver resolver(index.value());
		const std::expected<ResolvedPackageGraph, std::string> resolved_graph = resolver.Resolve(configuration.m_dependency_constraints);
		if (!resolved_graph.has_value())
		{
			return std::unexpected(resolved_graph.error());
		}

		const std::expected<ResolvedPackageGraph, std::string> localized_graph = InstallPackagesLocally(configuration, resolved_graph.value());
		if (!localized_graph.has_value())
		{
			return std::unexpected(localized_graph.error());
		}

		const std::expected<void, std::string> lockfile_write = WriteLockfile(localized_graph.value(), environment.m_lockfile_path, manifest_checksum.value());
		if (!lockfile_write.has_value())
		{
			return std::unexpected(lockfile_write.error());
		}

		environment.m_graph = localized_graph.value();
		environment.m_search_paths = BuildSearchPaths(configuration, environment.m_graph, env_paths);
		return environment;
	}

	std::expected<void, std::string> RemoveUnusedInstalledPackages(
		const MidoriProject::ManifestConfiguration& configuration,
		const ResolvedPackageGraph& active_graph,
		std::string_view package_name_filter)
	{
		const std::filesystem::path packages_directory = ResolveConfiguredDirectory(configuration, configuration.m_packages_dir, "packages");
		std::error_code error_code;
		if (!std::filesystem::exists(packages_directory, error_code) || !std::filesystem::is_directory(packages_directory, error_code))
		{
			return {};
		}

		std::unordered_set<std::string> active_versions;
		for (const auto& [package_name, package] : active_graph.m_packages)
		{
			active_versions.emplace(package_name + "@" + package.m_manifest.GetInfo().m_version);
		}

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(packages_directory))
		{
			if (!entry.is_directory())
			{
				continue;
			}

			const std::expected<PackageManifest, std::string> manifest = PackageManifest::LoadWithError(entry.path());
			if (!manifest.has_value())
			{
				continue;
			}

			if (!package_name_filter.empty() && manifest->GetInfo().m_name != package_name_filter)
			{
				continue;
			}

			const std::string key = manifest->GetInfo().m_name + "@" + manifest->GetInfo().m_version;
			if (active_versions.contains(key))
			{
				continue;
			}

			std::filesystem::remove_all(entry.path(), error_code);
			if (error_code)
			{
				return std::unexpected(std::format(
					"Failed to remove package directory '{}': {}",
					entry.path().string(),
					error_code.message()));
			}
		}

		return {};
	}

	std::string RenderDependencyTree(const ResolvedPackageGraph& graph)
	{
		std::string output;
		std::unordered_set<std::string> visiting;

		const std::function<void(const std::string&, const std::string&)> render_package =
			[&](const std::string& package_name, const std::string& indent)
			{
				if (!graph.m_packages.contains(package_name))
				{
					output.append(std::format("{}{} (missing)\n", indent, package_name));
					return;
				}

				const ResolvedPackage& package = graph.m_packages.at(package_name);
				output.append(std::format("{}{}@{}\n", indent, package_name, package.m_manifest.GetInfo().m_version));

				if (visiting.contains(package_name))
				{
					output.append(std::format("{}  (cycle)\n", indent));
					return;
				}

				visiting.emplace(package_name);
				for (const std::string& dependency : package.m_dependencies)
				{
					render_package(dependency, indent + "  ");
				}
				visiting.erase(package_name);
			};

		for (const std::string& root_dependency : graph.m_root_dependencies)
		{
			render_package(root_dependency, "");
		}

		return output;
	}
}
