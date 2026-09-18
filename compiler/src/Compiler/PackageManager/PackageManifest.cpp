#include "PackageManifest.h"

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Printer/Printer.h"
#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"

#include <format>
#include <toml.hpp>
#include <utility>

namespace
{
	constexpr const char* kManifestFileName = "package.marmot";

	const toml::value* FindTable(const toml::value& data, const char* key)
	{
		if (!data.is_table())
		{
			return nullptr;
		}

		if (!data.contains(key))
		{
			return nullptr;
		}

		return &data.at(key);
	}

	std::vector<std::string> ReadStringArray(const toml::value& value)
	{
		std::vector<std::string> items;
		if (!value.is_array())
		{
			return items;
		}

		items.reserve(value.as_array().size());

		for (const toml::value& item : value.as_array())
		{
			items.push_back(item.as_string());
		}

		return items;
	}

	std::unordered_map<std::string, std::string> ReadStringTable(const toml::value& value)
	{
		std::unordered_map<std::string, std::string> entries;
		for (const std::pair<const std::string, toml::value>& entry : value.as_table())
		{
			entries.emplace(entry.first, entry.second.as_string());
		}

		return entries;
	}

	std::expected<PackageInfo, std::string> ParsePackageInfo(const toml::value& data)
	{
		PackageInfo info;
		const toml::value* package_table = FindTable(data, "package");
		if (package_table == nullptr)
		{
			return std::unexpected("Missing [package] table.");
		}

		const toml::value& pkg = *package_table;
		info.m_name = toml::find_or<std::string>(pkg, "name", info.m_name);
		info.m_version = toml::find_or<std::string>(pkg, "version", info.m_version);
		info.m_description = toml::find_or<std::string>(pkg, "description", info.m_description);
		info.m_license = toml::find_or<std::string>(pkg, "license", info.m_license);
		info.m_repository = toml::find_or<std::string>(pkg, "repository", info.m_repository);
		info.m_midori_version = toml::find_or<std::string>(pkg, "marmot_version", info.m_midori_version);

		if (pkg.contains("authors"))
		{
			info.m_authors = ReadStringArray(pkg.at("authors"));
		}

		if (info.m_name.empty())
		{
			return std::unexpected("Missing required package name.");
		}

		const std::expected<MidoriVersion::SemanticVersion, std::string> version = MidoriVersion::SemanticVersion::Parse(info.m_version);
		if (!version.has_value())
		{
			return std::unexpected(std::format("Invalid package version '{}': {}", info.m_version, version.error()));
		}
		info.m_semantic_version = version.value();

		const std::expected<MidoriVersion::VersionConstraint, std::string> compiler_constraint =
			MidoriVersion::VersionConstraint::Parse(info.m_midori_version);
		if (!compiler_constraint.has_value())
		{
			return std::unexpected(std::format("Invalid marmot_version constraint '{}': {}", info.m_midori_version, compiler_constraint.error()));
		}
		info.m_midori_version_constraint = compiler_constraint.value();

		const std::expected<MidoriVersion::SemanticVersion, std::string> compiler_version =
			MidoriVersion::SemanticVersion::Parse(MidoriBuild::VersionString);
		if (!compiler_version.has_value())
		{
			return std::unexpected(std::format("Invalid compiler version '{}': {}", MidoriBuild::VersionString, compiler_version.error()));
		}

		if (!info.m_midori_version_constraint.Matches(compiler_version.value()))
		{
			return std::unexpected(std::format(
				"Package '{}' requires Marmot {}, but the current compiler version is {}.",
				info.m_name,
				info.m_midori_version,
				compiler_version->ToString()));
		}

		return info;
	}

	PackageModules ParsePackageModules(const toml::value& data)
	{
		PackageModules modules;
		const toml::value* package_table = FindTable(data, "package");
		if (package_table == nullptr)
		{
			return modules;
		}

		const toml::value* modules_table = FindTable(*package_table, "modules");
		if (modules_table == nullptr)
		{
			return modules;
		}

		modules.m_main = toml::find_or<std::string>(*modules_table, "main", modules.m_main);
		if (modules_table->contains("exports"))
		{
			modules.m_exports = ReadStringArray(modules_table->at("exports"));
		}

		return modules;
	}

	std::expected<PackageDependencies, std::string> ParsePackageDependencies(const toml::value& data)
	{
		PackageDependencies dependencies;
		const toml::value* deps_table = FindTable(data, "dependencies");
		if (deps_table == nullptr)
		{
			return dependencies;
		}

		dependencies.m_dependencies = ReadStringTable(*deps_table);
		for (const auto& [package_name, raw_constraint] : dependencies.m_dependencies)
		{
			const std::expected<MidoriVersion::VersionConstraint, std::string> constraint =
				MidoriVersion::VersionConstraint::Parse(raw_constraint);
			if (!constraint.has_value())
			{
				return std::unexpected(std::format(
					"Invalid dependency constraint for '{}': {}",
					package_name,
					constraint.error()));
			}
			dependencies.m_constraints.emplace(package_name, constraint.value());
		}

		return dependencies;
	}

	std::expected<PackageFFI, std::string> ParsePackageFFI(const toml::value& data)
	{
		PackageFFI ffi;
		const toml::value* ffi_table = FindTable(data, "ffi");
		if (ffi_table == nullptr)
		{
			return ffi;
		}

		ffi.m_enabled = toml::find_or<bool>(*ffi_table, "enabled", ffi.m_enabled);
		ffi.m_libraryName = toml::find_or<std::string>(*ffi_table, "library_name", ffi.m_libraryName);
		ffi.m_abi_version = toml::find_or<int>(*ffi_table, "abi_version", ffi.m_abi_version);
		ffi.m_thread_safe = toml::find_or<bool>(*ffi_table, "thread_safe", ffi.m_thread_safe);

		if (ffi_table->contains("functions"))
		{
			ffi.m_functions = ReadStringTable(ffi_table->at("functions"));
		}

		if (ffi.m_abi_version <= 0)
		{
			return std::unexpected(std::format(
				"Invalid [ffi].abi_version '{}'. Expected a positive integer.",
				ffi.m_abi_version));
		}

		return ffi;
	}

	PackageBuild ParsePackageBuild(const toml::value& data)
	{
		PackageBuild build;
		const toml::value* build_table = FindTable(data, "build");
		if (build_table == nullptr)
		{
			return build;
		}

		build.m_cmakeMinimumVersion = toml::find_or<std::string>(*build_table, "cmake_minimum_version", build.m_cmakeMinimumVersion);
		build.m_cppStandard = toml::find_or<std::string>(*build_table, "cpp_standard", build.m_cppStandard);
		return build;
	}

	std::optional<PrebuiltBinary> ParsePrebuiltBinary(const toml::value& prebuilt_table, const char* key)
	{
		if (!prebuilt_table.contains(key))
		{
			return std::nullopt;
		}

		const toml::value& table = prebuilt_table.at(key);
		PrebuiltBinary binary;
		binary.m_path = toml::find_or<std::string>(table, "path", "");
		binary.m_checksum = toml::find_or<std::string>(table, "checksum", "");
		return binary;
	}

	PackagePrebuilt ParsePackagePrebuilt(const toml::value& data)
	{
		PackagePrebuilt prebuilt;
		const toml::value* prebuilt_table = FindTable(data, "prebuilt");
		if (prebuilt_table == nullptr)
		{
			return prebuilt;
		}

		prebuilt.m_windowsX64 = ParsePrebuiltBinary(*prebuilt_table, "windows_x64");
		prebuilt.m_linuxX86_64 = ParsePrebuiltBinary(*prebuilt_table, "linux_x86_64");
		prebuilt.m_macosArm64 = ParsePrebuiltBinary(*prebuilt_table, "macos_arm64");
		prebuilt.m_macosX86_64 = ParsePrebuiltBinary(*prebuilt_table, "macos_x86_64");
		return prebuilt;
	}

	std::expected<PackageManifest, std::string> BuildManifest(const std::filesystem::path& packageDirectory, const toml::value& data)
	{
		const std::expected<PackageInfo, std::string> info = ParsePackageInfo(data);
		if (!info.has_value())
		{
			return std::unexpected(info.error());
		}

		const std::expected<PackageDependencies, std::string> dependencies = ParsePackageDependencies(data);
		if (!dependencies.has_value())
		{
			return std::unexpected(dependencies.error());
		}

		const std::expected<PackageFFI, std::string> ffi = ParsePackageFFI(data);
		if (!ffi.has_value())
		{
			return std::unexpected(ffi.error());
		}

		if (ffi->m_enabled && ffi->m_abi_version != MidoriFFIRegistry::ABI_VERSION)
		{
			return std::unexpected(std::format(
				"Package '{}' targets FFI ABI v{}, but this Marmot runtime supports FFI ABI v{}.",
				info->m_name,
				ffi->m_abi_version,
				MidoriFFIRegistry::ABI_VERSION));
		}

		return PackageManifest::Create(packageDirectory)
			.WithInfo(info.value())
			.WithModules(ParsePackageModules(data))
			.WithDependencies(dependencies.value())
			.WithFFI(ffi.value())
			.WithBuild(ParsePackageBuild(data))
			.WithPrebuilt(ParsePackagePrebuilt(data));
	}

	std::optional<std::filesystem::path> SelectPrebuiltLibraryPath(
		const PackagePrebuilt& prebuilt,
		const std::filesystem::path& packageDirectory)
	{
#ifdef _WIN32
		if (prebuilt.m_windowsX64.has_value())
		{
			return packageDirectory / prebuilt.m_windowsX64->m_path;
		}
#elif defined(__APPLE__)
		#if defined(__aarch64__) || defined(_M_ARM64)
			if (prebuilt.m_macosArm64.has_value())
			{
				return packageDirectory / prebuilt.m_macosArm64->m_path;
			}
		#else
			if (prebuilt.m_macosX86_64.has_value())
			{
				return packageDirectory / prebuilt.m_macosX86_64->m_path;
			}
		#endif
#else
		if (prebuilt.m_linuxX86_64.has_value())
		{
			return packageDirectory / prebuilt.m_linuxX86_64->m_path;
		}
#endif
		return std::nullopt;
	}
}

PackageManifest PackageManifest::Create(std::filesystem::path packageDirectory)
{
	PackageManifest manifest;
	manifest.m_packageDirectory = std::move(packageDirectory);
	return manifest;
}

PackageManifest PackageManifest::WithInfo(PackageInfo info) &&
{
	m_info = std::move(info);
	return std::move(*this);
}

PackageManifest PackageManifest::WithModules(PackageModules modules) &&
{
	m_modules = std::move(modules);
	return std::move(*this);
}

PackageManifest PackageManifest::WithDependencies(PackageDependencies dependencies) &&
{
	m_dependencies = std::move(dependencies);
	return std::move(*this);
}

PackageManifest PackageManifest::WithFFI(PackageFFI ffi) &&
{
	m_ffi = std::move(ffi);
	return std::move(*this);
}

PackageManifest PackageManifest::WithBuild(PackageBuild build) &&
{
	m_build = std::move(build);
	return std::move(*this);
}

PackageManifest PackageManifest::WithPrebuilt(PackagePrebuilt prebuilt) &&
{
	m_prebuilt = std::move(prebuilt);
	return std::move(*this);
}

std::optional<PackageManifest> PackageManifest::Load(const std::filesystem::path& packageDirectory)
{
	const std::expected<PackageManifest, std::string> manifest = LoadWithError(packageDirectory);
	if (!manifest.has_value())
	{
		Printer::PrintFormatted<Printer::Color::RED>("[PackageManifest] {}\n", manifest.error());
		return std::nullopt;
	}

	return manifest.value();
}

std::expected<PackageManifest, std::string> PackageManifest::LoadWithError(const std::filesystem::path& packageDirectory)
{
	const std::filesystem::path manifest_path = packageDirectory / kManifestFileName;
	if (!std::filesystem::exists(manifest_path))
	{
		return std::unexpected(std::format("package.marmot not found in: {}", packageDirectory.string()));
	}

	try
	{
		const toml::value data = toml::parse(manifest_path);
		return BuildManifest(packageDirectory, data);
	}
	catch (const std::exception& e)
	{
		return std::unexpected(std::format("Failed to parse package.marmot in {}: {}", packageDirectory.string(), e.what()));
	}
}

std::filesystem::path PackageManifest::GetMainModulePath() const
{
	if (m_modules.m_main.empty())
	{
		return {};
	}

	return m_packageDirectory / m_modules.m_main;
}

std::filesystem::path PackageManifest::GetFFILibraryPath() const
{
	if (!m_ffi.m_enabled || m_ffi.m_libraryName.empty())
	{
		return {};
	}

	const std::optional<std::filesystem::path> prebuilt_path = SelectPrebuiltLibraryPath(m_prebuilt, m_packageDirectory);
	if (prebuilt_path.has_value())
	{
		return prebuilt_path.value();
	}

#ifdef _WIN32
	return m_packageDirectory / "lib" / "windows" / "x64" / (m_ffi.m_libraryName + ".dll");
#elif defined(__APPLE__)
	return m_packageDirectory / "lib" / "macos" / ("lib" + m_ffi.m_libraryName + ".dylib");
#else
	return m_packageDirectory / "lib" / "linux" / "x86_64" / ("lib" + m_ffi.m_libraryName + ".so");
#endif
}

std::optional<PrebuiltBinary> PackageManifest::GetSelectedPrebuiltBinary() const
{
#ifdef _WIN32
	return m_prebuilt.m_windowsX64;
#elif defined(__APPLE__)
	#if defined(__aarch64__) || defined(_M_ARM64)
		return m_prebuilt.m_macosArm64;
	#else
		return m_prebuilt.m_macosX86_64;
	#endif
#else
	return m_prebuilt.m_linuxX86_64;
#endif
}

const PackageInfo& PackageManifest::GetInfo() const
{
	return m_info;
}

const PackageModules& PackageManifest::GetModules() const
{
	return m_modules;
}

const PackageDependencies& PackageManifest::GetDependencies() const
{
	return m_dependencies;
}

const PackageFFI& PackageManifest::GetFFI() const
{
	return m_ffi;
}

const PackageBuild& PackageManifest::GetBuild() const
{
	return m_build;
}

const PackagePrebuilt& PackageManifest::GetPrebuilt() const
{
	return m_prebuilt;
}

const std::filesystem::path& PackageManifest::GetPackageDirectory() const
{
	return m_packageDirectory;
}
