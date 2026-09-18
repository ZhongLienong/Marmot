#pragma once

#include "Common/Version/Version.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MidoriProject
{
	struct TestConfiguration
	{
		std::filesystem::path m_directory = "test";
		int m_timeout_ms = 30000;
	};

	struct ManifestConfiguration
	{
		std::filesystem::path m_root;
		std::filesystem::path m_manifest_path;
		std::string m_name;
		std::filesystem::path m_entry;
		std::filesystem::path m_source_dir;
		std::filesystem::path m_packages_dir;
		std::filesystem::path m_prelude_dir;
		std::vector<std::filesystem::path> m_extra_paths;
		std::unordered_map<std::string, std::string> m_dependencies;
		std::unordered_map<std::string, MidoriVersion::VersionConstraint> m_dependency_constraints;
		TestConfiguration m_test;
	};

	[[nodiscard]] std::optional<ManifestConfiguration> FindManifestConfiguration(const std::filesystem::path& input_path);

	void ApplyProjectManifestToEnvironment(const std::filesystem::path& input_path);

	bool InitializeProject(const std::filesystem::path& target_dir, std::string_view project_name, std::string& error_message);

	bool AddDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string_view constraint, std::string& error_message);

	bool RemoveDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string& error_message);
}

namespace MidoriPackage
{
	bool InitializePackage(const std::filesystem::path& target_dir, std::string_view package_name, std::string& error_message);
}
