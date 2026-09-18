#pragma once

#include "Compiler/PackageManager/PackageResolver.h"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace MidoriPackageManager
{
	struct LockfileLoadResult
	{
		ResolvedPackageGraph m_graph;
		bool m_manifest_matches = false;
		bool m_packages_available = true;
		std::vector<std::string> m_warnings;
	};

	[[nodiscard]] std::expected<void, std::string> WriteLockfile(
		const ResolvedPackageGraph& graph,
		const std::filesystem::path& lockfile_path,
		std::string_view manifest_checksum);

	[[nodiscard]] std::expected<LockfileLoadResult, std::string> ReadLockfile(
		const std::filesystem::path& lockfile_path,
		std::string_view expected_manifest_checksum);
}
