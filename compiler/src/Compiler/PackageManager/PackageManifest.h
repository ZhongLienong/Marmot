#pragma once

#include "Common/Version/Version.h"

#include <filesystem>
#include <expected>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct PackageInfo
{
	std::string m_name;
	std::string m_version = "0.0.0";
	MidoriVersion::SemanticVersion m_semantic_version;
	std::vector<std::string> m_authors;
	std::string m_description;
	std::string m_license;
	std::string m_repository;
	std::string m_midori_version = ">=1.0.0";
	MidoriVersion::VersionConstraint m_midori_version_constraint;
};

struct PackageModules
{
	std::string m_main;
	std::vector<std::string> m_exports;
};

struct PackageDependencies
{
	std::unordered_map<std::string, std::string> m_dependencies;
	std::unordered_map<std::string, MidoriVersion::VersionConstraint> m_constraints;
};

struct PackageFFI
{
	std::string m_libraryName;
	std::unordered_map<std::string, std::string> m_functions;
	int m_abi_version = 1;
	bool m_enabled = false;
	bool m_thread_safe = false;
};

struct PackageBuild
{
	std::string m_cmakeMinimumVersion = "3.24";
	std::string m_cppStandard = "23";
};

struct PrebuiltBinary
{
	std::filesystem::path m_path;
	std::string m_checksum;
};

struct PackagePrebuilt
{
	std::optional<PrebuiltBinary> m_windowsX64;
	std::optional<PrebuiltBinary> m_linuxX86_64;
	std::optional<PrebuiltBinary> m_macosArm64;
	std::optional<PrebuiltBinary> m_macosX86_64;
};

class PackageManifest
{
public:
	static std::optional<PackageManifest> Load(const std::filesystem::path& packageDirectory);
	static std::expected<PackageManifest, std::string> LoadWithError(const std::filesystem::path& packageDirectory);
	static PackageManifest Create(std::filesystem::path packageDirectory);

	PackageManifest WithInfo(PackageInfo info) &&;
	PackageManifest WithModules(PackageModules modules) &&;
	PackageManifest WithDependencies(PackageDependencies dependencies) &&;
	PackageManifest WithFFI(PackageFFI ffi) &&;
	PackageManifest WithBuild(PackageBuild build) &&;
	PackageManifest WithPrebuilt(PackagePrebuilt prebuilt) &&;

	const PackageInfo& GetInfo() const;
	const PackageModules& GetModules() const;
	const PackageDependencies& GetDependencies() const;
	const PackageFFI& GetFFI() const;
	const PackageBuild& GetBuild() const;
	const PackagePrebuilt& GetPrebuilt() const;

	const std::filesystem::path& GetPackageDirectory() const;
	std::filesystem::path GetMainModulePath() const;
	std::filesystem::path GetFFILibraryPath() const;
	std::optional<PrebuiltBinary> GetSelectedPrebuiltBinary() const;

private:
	PackageManifest() = default;

	PackageInfo m_info;
	PackageModules m_modules;
	PackageDependencies m_dependencies;
	PackageFFI m_ffi;
	PackageBuild m_build;
	PackagePrebuilt m_prebuilt;
	std::filesystem::path m_packageDirectory;
};
