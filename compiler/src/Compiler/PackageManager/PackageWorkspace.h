#pragma once

#include "Compiler/PackageManager/PackageResolver.h"
#include "Utility/Project/ProjectManifest.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace MidoriPackageManager
{
	enum class ResolveMode
	{
		PreferLockfile,
		ForceRefresh
	};

	struct PackageEnvironment
	{
		ResolvedPackageGraph m_graph;
		std::vector<std::filesystem::path> m_search_paths;
		std::vector<std::string> m_warnings;
		bool m_used_lockfile = false;
		std::filesystem::path m_lockfile_path;
	};

	[[nodiscard]] std::filesystem::path GetGlobalPackageCacheDirectory();

	[[nodiscard]] std::expected<ResolvedPackageGraph, std::string> InstallPackagesLocally(
		const MidoriProject::ManifestConfiguration& configuration,
		const ResolvedPackageGraph& graph);

	[[nodiscard]] std::expected<MidoriVersion::SemanticVersion, std::string> FindLatestAvailableVersion(
		const MidoriProject::ManifestConfiguration& configuration,
		std::string_view package_name);

	[[nodiscard]] std::expected<PackageEnvironment, std::string> PreparePackageEnvironment(
		const MidoriProject::ManifestConfiguration& configuration,
		ResolveMode mode = ResolveMode::PreferLockfile);

	[[nodiscard]] std::expected<void, std::string> RemoveUnusedInstalledPackages(
		const MidoriProject::ManifestConfiguration& configuration,
		const ResolvedPackageGraph& active_graph,
		std::string_view package_name_filter = {});

	[[nodiscard]] std::string RenderDependencyTree(const ResolvedPackageGraph& graph);
}
