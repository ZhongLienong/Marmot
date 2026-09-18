#pragma once

#include "Compiler/PackageManager/PackageIndex.h"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MidoriPackageManager
{
	struct ResolvedPackage
	{
		PackageManifest m_manifest = PackageManifest::Create({});
		std::vector<std::string> m_dependencies;
		std::string m_source;
		std::string m_package_checksum;
	};

	struct ResolvedPackageGraph
	{
		std::unordered_map<std::string, ResolvedPackage> m_packages;
		std::vector<std::string> m_root_dependencies;

		[[nodiscard]] const ResolvedPackage* Find(std::string_view package_name) const;

		[[nodiscard]] std::vector<std::string> TopologicalOrder() const;

		[[nodiscard]] std::vector<std::filesystem::path> PackageSearchPaths() const;
	};

	class PackageResolver
	{
	public:
		explicit PackageResolver(const PackageIndex& index);

		[[nodiscard]] std::expected<ResolvedPackageGraph, std::string> Resolve(
			const std::unordered_map<std::string, MidoriVersion::VersionConstraint>& root_dependencies) const;

	private:
		const PackageIndex& m_index;

		[[nodiscard]] std::expected<void, std::string> ResolvePackage(
			ResolvedPackageGraph& graph,
			const std::string& package_name,
			const MidoriVersion::VersionConstraint& constraint,
			const std::string& required_by,
			std::vector<std::string>& stack) const;
	};
}
