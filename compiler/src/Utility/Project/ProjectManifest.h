#pragma once

#include "Common/Version/Version.h"
#include "Compiler/CompilationInputs/CompilationInputs.h"

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

	// The directories in MARMOT_PATH, in order.
	[[nodiscard]] std::vector<std::filesystem::path> EnvironmentSearchPaths();

	// The native package described by `directory`/package.marmot: one whose
	// [ffi] section is enabled. Anything else, including a manifest that does
	// not parse, is no native package.
	[[nodiscard]] std::optional<NativePackage> ReadNativePackage(const std::filesystem::path& directory);

	// Compiler inputs when no project applies: MARMOT_PATH, and native packages
	// read from package.marmot files as imports reach them.
	[[nodiscard]] CompilationInputs EnvironmentCompilationInputs();

	// Compiler inputs for `input_path` as the CLI has always found them: the
	// project or package manifest above it, its resolved dependencies, the
	// prelude, then MARMOT_PATH. Without a manifest, EnvironmentCompilationInputs.
	// This is the discovery a build plan replaces.
	[[nodiscard]] CompilationInputs DiscoverCompilationInputs(const std::filesystem::path& input_path);

	bool InitializeProject(const std::filesystem::path& target_dir, std::string_view project_name, std::string& error_message);

	bool AddDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string_view constraint, std::string& error_message);

	bool RemoveDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string& error_message);
}

namespace MidoriPackage
{
	bool InitializePackage(const std::filesystem::path& target_dir, std::string_view package_name, std::string& error_message);
}
