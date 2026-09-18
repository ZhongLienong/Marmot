#pragma once

#include "Compiler/PackageManager/PackageManifest.h"

#include <expected>
#include <filesystem>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MidoriPackageManager
{
	struct PackageIndexEntry
	{
		PackageManifest m_manifest;
		size_t m_priority = 0u;
	};

	class PackageIndex
	{
	public:
		[[nodiscard]] static std::expected<PackageIndex, std::string> Scan(const std::vector<std::filesystem::path>& roots);

		[[nodiscard]] std::vector<PackageIndexEntry> FindPackages(std::string_view package_name) const;

		[[nodiscard]] std::vector<PackageIndexEntry> FindPackagesMatching(std::string_view package_name, const MidoriVersion::VersionConstraint& constraint) const;

	private:
		std::unordered_map<std::string, std::vector<PackageIndexEntry>> m_packages;
	};
}
