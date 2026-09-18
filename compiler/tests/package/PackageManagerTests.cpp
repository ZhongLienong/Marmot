#include <catch2/catch_test_macros.hpp>

#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"
#include "Common/Version/Version.h"
#include "Compiler/PackageManager/PackageManifest.h"
#include "Compiler/PackageManager/PackageWorkspace.h"
#include "Utility/Project/ProjectManifest.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"

#include <filesystem>
#include <optional>
#include <string>

TEST_CASE("SemanticVersion parses prerelease and build metadata", "[package][version]")
{
	const std::expected<MidoriVersion::SemanticVersion, std::string> version =
		MidoriVersion::SemanticVersion::Parse("1.2.3-alpha.1+build.42");

	REQUIRE(version.has_value());
	CHECK(version->m_major == 1);
	CHECK(version->m_minor == 2);
	CHECK(version->m_patch == 3);
	REQUIRE(version->m_prerelease == std::vector<std::string>{ "alpha", "1" });
	REQUIRE(version->m_build == std::vector<std::string>{ "build", "42" });
	CHECK(version->ToString() == "1.2.3-alpha.1+build.42");

	const std::expected<MidoriVersion::SemanticVersion, std::string> release =
		MidoriVersion::SemanticVersion::Parse("1.2.3");
	REQUIRE(release.has_value());
	CHECK(*version < *release);
}

TEST_CASE("VersionConstraint matches caret tilde and explicit comparisons", "[package][version]")
{
	const std::expected<MidoriVersion::SemanticVersion, std::string> version_1_4_0 =
		MidoriVersion::SemanticVersion::Parse("1.4.0");
	const std::expected<MidoriVersion::SemanticVersion, std::string> version_2_0_0 =
		MidoriVersion::SemanticVersion::Parse("2.0.0");
	const std::expected<MidoriVersion::SemanticVersion, std::string> version_1_2_5 =
		MidoriVersion::SemanticVersion::Parse("1.2.5");

	REQUIRE(version_1_4_0.has_value());
	REQUIRE(version_2_0_0.has_value());
	REQUIRE(version_1_2_5.has_value());

	const std::expected<MidoriVersion::VersionConstraint, std::string> caret =
		MidoriVersion::VersionConstraint::Parse("^1.2.3");
	const std::expected<MidoriVersion::VersionConstraint, std::string> tilde =
		MidoriVersion::VersionConstraint::Parse("~1.2.3");
	const std::expected<MidoriVersion::VersionConstraint, std::string> range =
		MidoriVersion::VersionConstraint::Parse(">=1.0.0, <2.0.0");

	REQUIRE(caret.has_value());
	REQUIRE(tilde.has_value());
	REQUIRE(range.has_value());

	CHECK(caret->Matches(*version_1_4_0));
	CHECK_FALSE(caret->Matches(*version_2_0_0));
	CHECK(tilde->Matches(*version_1_2_5));
	CHECK_FALSE(tilde->Matches(*version_1_4_0));
	CHECK(range->Matches(*version_1_4_0));
	CHECK_FALSE(range->Matches(*version_2_0_0));
}

TEST_CASE("PackageManifest validates dependency constraints", "[package][manifest]")
{
	const MidoriTest::TempProject project(
		{
			MidoriTest::TempProjectFile(
				"pkg/package.marmot",
				"[package]\n"
				"name = \"Image\"\n"
				"version = \"0.2.0\"\n"
				"marmot_version = \">=1.0.0\"\n"
				"\n"
				"[package.modules]\n"
				"main = \"Image.mmt\"\n"
				"exports = [\"Image\"]\n"
				"\n"
				"[dependencies]\n"
				"Collections = \"^1.2.0\"\n"),
			MidoriTest::TempProjectFile("pkg/Image.mmt", "module Image\n")
		});

	const std::optional<PackageManifest> manifest = PackageManifest::Load(project.Path("pkg"));
	REQUIRE(manifest.has_value());
	CHECK(manifest->GetInfo().m_version == "0.2.0");
	CHECK(manifest->GetInfo().m_semantic_version.ToString() == "0.2.0");
	CHECK(manifest->GetDependencies().m_constraints.contains("Collections"));
	CHECK(manifest->GetDependencies().m_constraints.at("Collections").Matches(
		MidoriVersion::SemanticVersion::Parse("1.4.0").value()));
}

TEST_CASE("PackageManifest reads ffi abi metadata", "[package][manifest]")
{
	const MidoriTest::TempProject project(
		{
			MidoriTest::TempProjectFile(
				"pkg/package.marmot",
				"[package]\n"
				"name = \"Image\"\n"
				"version = \"0.2.0\"\n"
				"marmot_version = \">=1.0.0\"\n"
				"\n"
				"[ffi]\n"
				"enabled = true\n"
				"library_name = \"marmot_image\"\n"
				"abi_version = 1\n"
				"\n"
				"[ffi.functions]\n"
				"\"Image::ReadInfo\" = \"marmot_image_read_info\"\n"),
			MidoriTest::TempProjectFile("pkg/Image.mmt", "module Image\n")
		});

	const std::expected<PackageManifest, std::string> manifest = PackageManifest::LoadWithError(project.Path("pkg"));
	REQUIRE(manifest.has_value());
	CHECK(manifest->GetFFI().m_enabled);
	CHECK(manifest->GetFFI().m_libraryName == "marmot_image");
	CHECK(manifest->GetFFI().m_abi_version == MidoriFFIRegistry::ABI_VERSION);
	REQUIRE(manifest->GetFFI().m_functions.contains("Image::ReadInfo"));
	CHECK(manifest->GetFFI().m_functions.at("Image::ReadInfo") == "marmot_image_read_info");
}

TEST_CASE("PackageManifest rejects unsupported ffi abi versions", "[package][manifest]")
{
	const int unsupported_abi_version = MidoriFFIRegistry::ABI_VERSION + 1;
	const MidoriTest::TempProject project(
		{
			MidoriTest::TempProjectFile(
				"pkg/package.marmot",
				"[package]\n"
				"name = \"Image\"\n"
				"version = \"0.2.0\"\n"
				"marmot_version = \">=1.0.0\"\n"
				"\n"
				"[ffi]\n"
				"enabled = true\n"
				"library_name = \"marmot_image\"\n"
				"abi_version = " + std::to_string(unsupported_abi_version) + "\n"),
			MidoriTest::TempProjectFile("pkg/Image.mmt", "module Image\n")
		});

	const std::expected<PackageManifest, std::string> manifest = PackageManifest::LoadWithError(project.Path("pkg"));
	REQUIRE_FALSE(manifest.has_value());
	CHECK(manifest.error().find("targets FFI ABI v") != std::string::npos);
	CHECK(manifest.error().find(std::to_string(unsupported_abi_version)) != std::string::npos);
	CHECK(manifest.error().find(std::to_string(MidoriFFIRegistry::ABI_VERSION)) != std::string::npos);
}

TEST_CASE("Project package environment resolves local packages and reuses the lockfile", "[package][workspace]")
{
	const MidoriTest::ScopedEnvVar clear_midori_path("MARMOT_PATH", std::nullopt);
	const MidoriTest::TempProject project(
		{
			MidoriTest::TempProjectFile(
				"project.marmot",
				"[project]\n"
				"name = \"App\"\n"
				"source_dir = \"src\"\n"
				"packages_dir = \"packages\"\n"
				"prelude_dir = \"MarmotPrelude\"\n"
				"marmot_path = [\"registry\"]\n"
				"\n"
				"[dependencies]\n"
				"Greeter = \"^1.0.0\"\n"),
			MidoriTest::TempProjectFile("src/Main.mmt", "module Main\n"),
			MidoriTest::TempProjectFile("MarmotPrelude/System.mmt", "module System\n"),
			MidoriTest::TempProjectFile(
				"registry/Greeter-1.0.0/package.marmot",
				"[package]\n"
				"name = \"Greeter\"\n"
				"version = \"1.0.0\"\n"
				"marmot_version = \">=1.0.0\"\n"
				"\n"
				"[package.modules]\n"
				"main = \"Greeter.mmt\"\n"
				"exports = [\"Greeter\"]\n"),
			MidoriTest::TempProjectFile("registry/Greeter-1.0.0/Greeter.mmt", "module Greeter\n"),
			MidoriTest::TempProjectFile(
				"registry/Greeter-1.2.0/package.marmot",
				"[package]\n"
				"name = \"Greeter\"\n"
				"version = \"1.2.0\"\n"
				"marmot_version = \">=1.0.0\"\n"
				"\n"
				"[package.modules]\n"
				"main = \"Greeter.mmt\"\n"
				"exports = [\"Greeter\"]\n"),
			MidoriTest::TempProjectFile("registry/Greeter-1.2.0/Greeter.mmt", "module Greeter\n")
		});

	const std::optional<MidoriProject::ManifestConfiguration> configuration =
		MidoriProject::FindManifestConfiguration(project.Root());
	REQUIRE(configuration.has_value());

	const std::expected<MidoriPackageManager::PackageEnvironment, std::string> first_environment =
		MidoriPackageManager::PreparePackageEnvironment(*configuration, MidoriPackageManager::ResolveMode::ForceRefresh);
	REQUIRE(first_environment.has_value());
	REQUIRE(first_environment->m_graph.m_packages.contains("Greeter"));
	CHECK(first_environment->m_graph.m_packages.at("Greeter").m_manifest.GetInfo().m_version == "1.2.0");
	CHECK(std::filesystem::exists(project.Path("packages/Greeter-1.2.0/package.marmot")));
	CHECK(std::filesystem::exists(project.Path("marmot.lock")));

	const std::expected<MidoriPackageManager::PackageEnvironment, std::string> second_environment =
		MidoriPackageManager::PreparePackageEnvironment(*configuration, MidoriPackageManager::ResolveMode::PreferLockfile);
	REQUIRE(second_environment.has_value());
	CHECK(second_environment->m_used_lockfile);
	CHECK(second_environment->m_graph.m_packages.at("Greeter").m_manifest.GetInfo().m_version == "1.2.0");
}

TEST_CASE("Project manifest dependency helpers update the active manifest", "[package][manifest]")
{
	const MidoriTest::TempProject project(
		{
			MidoriTest::TempProjectFile(
				"project.marmot",
				"[project]\n"
				"name = \"App\"\n"
				"source_dir = \"src\"\n"),
			MidoriTest::TempProjectFile("src/Main.mmt", "module Main\n")
		});

	std::string error_message;
	REQUIRE(MidoriProject::AddDependency(project.Root(), "Greeter", "^1.2.0", error_message));

	const std::optional<MidoriProject::ManifestConfiguration> after_add =
		MidoriProject::FindManifestConfiguration(project.Root());
	REQUIRE(after_add.has_value());
	CHECK(after_add->m_dependencies.contains("Greeter"));
	CHECK(after_add->m_dependencies.at("Greeter") == "^1.2.0");

	REQUIRE(MidoriProject::RemoveDependency(project.Root(), "Greeter", error_message));
	const std::optional<MidoriProject::ManifestConfiguration> after_remove =
		MidoriProject::FindManifestConfiguration(project.Root());
	REQUIRE(after_remove.has_value());
	CHECK_FALSE(after_remove->m_dependencies.contains("Greeter"));
}
