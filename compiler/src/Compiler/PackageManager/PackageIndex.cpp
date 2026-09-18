#include "Compiler/PackageManager/PackageIndex.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <system_error>
#include <unordered_set>

namespace
{
	[[nodiscard]] std::filesystem::path NormalizePath(const std::filesystem::path& path)
	{
		std::error_code error_code;
		std::filesystem::path absolute_path = path;
		if (!absolute_path.is_absolute())
		{
			absolute_path = std::filesystem::absolute(absolute_path, error_code);
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

	[[nodiscard]] std::vector<std::filesystem::path> DiscoverPackageDirectories(const std::filesystem::path& root)
	{
		std::vector<std::filesystem::path> directories;
		std::error_code error_code;
		if (!std::filesystem::exists(root, error_code) || !std::filesystem::is_directory(root, error_code))
		{
			return directories;
		}

		const std::filesystem::path manifest_path = root / "package.marmot";
		if (std::filesystem::exists(manifest_path, error_code))
		{
			directories.push_back(root);
		}

		for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root))
		{
			if (!entry.is_directory())
			{
				continue;
			}

			const std::filesystem::path child_manifest = entry.path() / "package.marmot";
			if (std::filesystem::exists(child_manifest, error_code))
			{
				directories.push_back(entry.path());
			}
		}

		return directories;
	}
}

namespace MidoriPackageManager
{
	std::expected<PackageIndex, std::string> PackageIndex::Scan(const std::vector<std::filesystem::path>& roots)
	{
		PackageIndex index;
		std::unordered_set<std::string> visited_directories;

		for (size_t priority = 0u; priority < roots.size(); priority += 1u)
		{
			const std::vector<std::filesystem::path> package_directories = DiscoverPackageDirectories(roots[priority]);
			for (const std::filesystem::path& package_directory : package_directories)
			{
				const std::string directory_key = NormalizePathKey(package_directory);
				if (visited_directories.contains(directory_key))
				{
					continue;
				}
				visited_directories.emplace(directory_key);

				const std::expected<PackageManifest, std::string> manifest = PackageManifest::LoadWithError(package_directory);
				if (!manifest.has_value())
				{
					return std::unexpected(manifest.error());
				}

				PackageIndexEntry entry
				{
					.m_manifest = manifest.value(),
					.m_priority = priority
				};
				index.m_packages[entry.m_manifest.GetInfo().m_name].push_back(std::move(entry));
			}
		}

		for (auto& [_, entries] : index.m_packages)
		{
			std::ranges::sort(
				entries,
				[](const PackageIndexEntry& left, const PackageIndexEntry& right)
				{
					const MidoriVersion::SemanticVersion& left_version = left.m_manifest.GetInfo().m_semantic_version;
					const MidoriVersion::SemanticVersion& right_version = right.m_manifest.GetInfo().m_semantic_version;
					if (left_version != right_version)
					{
						return left_version > right_version;
					}

					if (left.m_priority != right.m_priority)
					{
						return left.m_priority < right.m_priority;
					}

					return left.m_manifest.GetPackageDirectory().generic_string() < right.m_manifest.GetPackageDirectory().generic_string();
				});
		}

		return index;
	}

	std::vector<PackageIndexEntry> PackageIndex::FindPackages(std::string_view package_name) const
	{
		const std::unordered_map<std::string, std::vector<PackageIndexEntry>>::const_iterator entry = m_packages.find(std::string(package_name));
		if (entry == m_packages.end())
		{
			return {};
		}

		return entry->second;
	}

	std::vector<PackageIndexEntry> PackageIndex::FindPackagesMatching(std::string_view package_name, const MidoriVersion::VersionConstraint& constraint) const
	{
		std::vector<PackageIndexEntry> matches;
		const std::unordered_map<std::string, std::vector<PackageIndexEntry>>::const_iterator entry = m_packages.find(std::string(package_name));
		if (entry == m_packages.end())
		{
			return matches;
		}

		for (const PackageIndexEntry& package : entry->second)
		{
			if (constraint.Matches(package.m_manifest.GetInfo().m_semantic_version))
			{
				matches.push_back(package);
			}
		}

		return matches;
	}
}
