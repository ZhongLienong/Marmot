#include "Utility/Project/ProjectManifest.h"

#include "Compiler/PackageManager/PackageWorkspace.h"
#include "Common/Printer/Printer.h"

#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <unordered_map>
#include <system_error>
#include <vector>

namespace
{
	constexpr const char* ProjectManifestFileName = "project.marmot";
	constexpr const char* PackageManifestFileName = "package.marmot";

	struct ProjectSettings
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
		MidoriProject::TestConfiguration m_test;
	};

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

	std::vector<std::filesystem::path> ReadPathArray(const toml::value& value)
	{
		std::vector<std::filesystem::path> paths;
		if (!value.is_array())
		{
			return paths;
		}

		paths.reserve(value.as_array().size());
		for (const toml::value& item : value.as_array())
		{
			if (item.is_string())
			{
				paths.emplace_back(item.as_string());
			}
		}

		return paths;
	}

	std::unordered_map<std::string, std::string> ReadStringTable(const toml::value& value)
	{
		std::unordered_map<std::string, std::string> entries;
		if (!value.is_table())
		{
			return entries;
		}

		for (const std::pair<const std::string, toml::value>& entry : value.as_table())
		{
			if (entry.second.is_string())
			{
				entries.emplace(entry.first, entry.second.as_string());
			}
		}

		return entries;
	}

	std::optional<ProjectSettings> LoadProjectSettings(const std::filesystem::path& manifest_path, const std::filesystem::path& root, bool report_missing_project)
	{
		try
		{
			const toml::value data = toml::parse(manifest_path);
			const toml::value* project_table = FindTable(data, "project");

			ProjectSettings settings;
			settings.m_root = root;
			settings.m_manifest_path = manifest_path;

			if (project_table != nullptr)
			{
				settings.m_name = toml::find_or<std::string>(*project_table, "name", settings.m_name);
				settings.m_entry = toml::find_or<std::string>(*project_table, "entry", "");
				settings.m_source_dir = toml::find_or<std::string>(*project_table, "source_dir", "");
				settings.m_packages_dir = toml::find_or<std::string>(*project_table, "packages_dir", "");
				settings.m_prelude_dir = toml::find_or<std::string>(*project_table, "prelude_dir", "");

				if (project_table->contains("marmot_path"))
				{
					settings.m_extra_paths = ReadPathArray(project_table->at("marmot_path"));
				}
			}
			else if (manifest_path.filename() == PackageManifestFileName)
			{
				const toml::value* package_table = FindTable(data, "package");
				if (package_table == nullptr)
				{
					return std::nullopt;
				}

				settings.m_name = toml::find_or<std::string>(*package_table, "name", settings.m_name);
				settings.m_source_dir = ".";
				settings.m_packages_dir = "packages";
				settings.m_prelude_dir = "MarmotPrelude";

				if (const toml::value* modules_table = FindTable(*package_table, "modules"))
				{
					settings.m_entry = toml::find_or<std::string>(*modules_table, "main", "");
				}
			}
			else
			{
				if (report_missing_project)
				{
					Printer::Print<Printer::Color::RED>(
						std::format("[ProjectManifest] Missing [project] table in {}\n", manifest_path.string()));
				}
				return std::nullopt;
			}

			if (const toml::value* dependencies_table = FindTable(data, "dependencies"))
			{
				settings.m_dependencies = ReadStringTable(*dependencies_table);
				for (const auto& [package_name, raw_constraint] : settings.m_dependencies)
				{
					const std::expected<MidoriVersion::VersionConstraint, std::string> constraint =
						MidoriVersion::VersionConstraint::Parse(raw_constraint);
					if (!constraint.has_value())
					{
						Printer::Print<Printer::Color::RED>(
							std::format(
								"[ProjectManifest] Invalid dependency constraint for '{}' in {}: {}\n",
								package_name,
								manifest_path.string(),
								constraint.error()));
						return std::nullopt;
					}
					settings.m_dependency_constraints.emplace(package_name, constraint.value());
				}
			}

			if (const toml::value* test_table = FindTable(data, "test"))
			{
				settings.m_test.m_directory = toml::find_or<std::string>(*test_table, "dir", settings.m_test.m_directory.string());
				settings.m_test.m_timeout_ms = toml::find_or<int>(*test_table, "timeout_ms", settings.m_test.m_timeout_ms);
			}

			return settings;
		}
		catch (const std::exception& e)
		{
			Printer::Print<Printer::Color::RED>(std::format("[ProjectManifest] Failed to parse {}: {}\n", manifest_path.string(), e.what()));
			return std::nullopt;
		}
	}

	std::optional<ProjectSettings> FindProjectSettings(const std::filesystem::path& start_dir)
	{
		std::filesystem::path current = start_dir;
		while (true)
		{
			const std::filesystem::path project_manifest = current / ProjectManifestFileName;
			if (std::filesystem::exists(project_manifest))
			{
				return LoadProjectSettings(project_manifest, current, true);
			}

			const std::filesystem::path package_manifest = current / PackageManifestFileName;
			if (std::filesystem::exists(package_manifest))
			{
				std::optional<ProjectSettings> settings = LoadProjectSettings(package_manifest, current, false);
				if (settings.has_value())
				{
					return settings;
				}
			}

			if (!current.has_parent_path())
			{
				break;
			}

			const std::filesystem::path parent = current.parent_path();
			if (parent == current)
			{
				break;
			}
			current = parent;
		}

		return std::nullopt;
	}

	std::filesystem::path ResolveInputDirectory(const std::filesystem::path& input_path)
	{
		std::filesystem::path start_dir = input_path;
		std::error_code ec;
		if (std::filesystem::is_directory(start_dir, ec))
		{
			return start_dir;
		}

		if (input_path.has_parent_path())
		{
			return input_path.parent_path();
		}

		return std::filesystem::current_path(ec);
	}

	std::optional<std::string> ReadEnvironmentVariable(const char* name)
	{
	#ifdef _WIN32
		char* value = nullptr;
		size_t len = 0u;
		if (_dupenv_s(&value, &len, name) != 0 || value == nullptr)
		{
			return std::nullopt;
		}

		std::string result(value);
		std::free(value);
		if (result.empty())
		{
			return std::nullopt;
		}

		return result;
	#else
		const char* value = std::getenv(name);
		if (value == nullptr || value[0] == '\0')
		{
			return std::nullopt;
		}

		return std::string(value);
	#endif
	}

	std::vector<std::filesystem::path> SplitSearchPaths(const std::string& path_str, char separator)
	{
		std::vector<std::filesystem::path> paths;
		size_t start = 0u;
		size_t end = path_str.find(separator);

		while (end != std::string::npos)
		{
			std::string segment = path_str.substr(start, end - start);
			if (!segment.empty())
			{
				paths.emplace_back(segment);
			}
			start = end + 1u;
			end = path_str.find(separator, start);
		}

		std::string last_segment = path_str.substr(start);
		if (!last_segment.empty())
		{
			paths.emplace_back(last_segment);
		}

		return paths;
	}

	std::filesystem::path NormalizePath(const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::path absolute = path;
		if (!path.is_absolute())
		{
			absolute = std::filesystem::absolute(path, ec);
			if (ec)
			{
				absolute = path;
			}
		}

		std::filesystem::path normalized = std::filesystem::weakly_canonical(absolute, ec);
		if (ec)
		{
			normalized = absolute.lexically_normal();
		}
		return normalized;
	}

	std::string NormalizePathString(const std::filesystem::path& path)
	{
		std::string value = NormalizePath(path).string();
	#ifdef _WIN32
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c));});
	#endif
		return value;
	}

	bool ContainsPath(const std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate)
	{
		const std::string normalized_candidate = NormalizePathString(candidate);
		for (const std::filesystem::path& path : paths)
		{
			if (NormalizePathString(path) == normalized_candidate)
			{
				return true;
			}
		}
		return false;
	}

	void AppendUniquePath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& candidate)
	{
		if (candidate.empty())
		{
			return;
		}

		std::error_code ec;
		if (!std::filesystem::exists(candidate, ec) || !std::filesystem::is_directory(candidate, ec))
		{
			return;
		}

		if (!ContainsPath(paths, candidate))
		{
			paths.push_back(candidate);
		}
	}

	std::vector<std::filesystem::path> BuildProjectSearchPaths(const ProjectSettings& settings)
	{
		std::vector<std::filesystem::path> paths;

		const std::filesystem::path default_source = settings.m_root / "src";
		const std::filesystem::path default_packages = settings.m_root / "packages";
		const std::filesystem::path default_prelude = settings.m_root / "MarmotPrelude";

		if (!settings.m_source_dir.empty())
		{
			AppendUniquePath(paths, settings.m_root / settings.m_source_dir);
		}
		else
		{
			AppendUniquePath(paths, default_source);
		}

		if (!settings.m_packages_dir.empty())
		{
			AppendUniquePath(paths, settings.m_root / settings.m_packages_dir);
		}
		else
		{
			AppendUniquePath(paths, default_packages);
		}

		for (const std::filesystem::path& extra : settings.m_extra_paths)
		{
			AppendUniquePath(paths, extra.is_absolute() ? extra : settings.m_root / extra);
		}

		if (!settings.m_prelude_dir.empty())
		{
			AppendUniquePath(paths, settings.m_root / settings.m_prelude_dir);
		}
		else
		{
			AppendUniquePath(paths, default_prelude);
		}

		return paths;
	}

	std::string JoinSearchPaths(const std::vector<std::filesystem::path>& paths, char separator)
	{
		std::string result;
		for (size_t i = 0uz; i < paths.size(); i += 1uz)
		{
			if (i != 0uz)
			{
				result.push_back(separator);
			}
			result.append(paths[i].string());
		}
		return result;
	}

	void SetEnvironmentVariable(const char* name, const std::string& value)
	{
	#ifdef _WIN32
		_putenv_s(name, value.c_str());
	#else
		setenv(name, value.c_str(), 1);
	#endif
	}

	std::string DeriveProjectName(const std::filesystem::path& root)
	{
		const std::string name = root.filename().string();
		if (!name.empty())
		{
			return name;
		}
		return "MidoriProject";
	}

	std::string DerivePackageName(const std::filesystem::path& root)
	{
		const std::string name = root.filename().string();
		if (!name.empty())
		{
			return name;
		}
		return "MidoriPackage";
	}

	std::string SanitizeModuleName(std::string_view name)
	{
		std::string result;
		result.reserve(name.size());

		for (char c : name)
		{
			const unsigned char uc = static_cast<unsigned char>(c);
			if (std::isalnum(uc) != 0 || c == '_')
			{
				result.push_back(c);
			}
			else
			{
				result.push_back('_');
			}
		}

		if (result.empty())
		{
			result = "Package";
		}

		const unsigned char first = static_cast<unsigned char>(result.front());
		if (std::isalpha(first) == 0 && result.front() != '_')
		{
			result.insert(0, "Package");
		}

		return result;
	}

	std::string EscapeTomlString(std::string_view value)
	{
		std::string result;
		result.reserve(value.size());
		for (char c : value)
		{
			switch (c)
			{
			case '\\':
				result.append("\\\\");
				break;
			case '"':
				result.append("\\\"");
				break;
			case '\n':
				result.append("\\n");
				break;
			case '\r':
				result.append("\\r");
				break;
			case '\t':
				result.append("\\t");
				break;
			default:
				result.push_back(c);
				break;
			}
		}
		return result;
	}

	bool WriteFile(const std::filesystem::path& path, std::string_view contents, std::string& error_message)
	{
		std::ofstream out(path, std::ios::binary);
		if (!out.is_open())
		{
			error_message = std::format("Failed to write file: {}", path.string());
			return false;
		}

		out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		if (!out)
		{
			error_message = std::format("Failed to write file: {}", path.string());
			return false;
		}

		return true;
	}

	std::optional<std::filesystem::path> ResolveManifestPathForEditing(const std::filesystem::path& input_path, std::string& error_message)
	{
		const std::optional<MidoriProject::ManifestConfiguration> configuration = MidoriProject::FindManifestConfiguration(input_path);
		if (!configuration.has_value())
		{
			error_message = "Could not find project.marmot or package.marmot.";
			return std::nullopt;
		}

		return configuration->m_manifest_path;
	}

	bool WriteTomlDocument(const std::filesystem::path& path, const toml::value& document, std::string& error_message)
	{
		return WriteFile(path, toml::format(document), error_message);
	}
}

namespace MidoriProject
{
	std::optional<ManifestConfiguration> FindManifestConfiguration(const std::filesystem::path& input_path)
	{
		const std::filesystem::path start_dir = ResolveInputDirectory(input_path);
		std::optional<ProjectSettings> settings = FindProjectSettings(start_dir);
		if (!settings.has_value())
		{
			return std::nullopt;
		}

		ManifestConfiguration configuration;
		configuration.m_root = settings->m_root;
		configuration.m_manifest_path = settings->m_manifest_path;
		configuration.m_name = settings->m_name;
		configuration.m_entry = settings->m_entry;
		configuration.m_source_dir = settings->m_source_dir;
		configuration.m_packages_dir = settings->m_packages_dir;
		configuration.m_prelude_dir = settings->m_prelude_dir;
		configuration.m_extra_paths = settings->m_extra_paths;
		configuration.m_dependencies = settings->m_dependencies;
		configuration.m_dependency_constraints = settings->m_dependency_constraints;
		configuration.m_test = settings->m_test;
		return configuration;
	}

	void ApplyProjectManifestToEnvironment(const std::filesystem::path& input_path)
	{
		const std::optional<ManifestConfiguration> configuration = FindManifestConfiguration(input_path);
		if (!configuration.has_value())
		{
			return;
		}

		const std::expected<MidoriPackageManager::PackageEnvironment, std::string> package_environment =
			MidoriPackageManager::PreparePackageEnvironment(*configuration);
		if (!package_environment.has_value())
		{
			Printer::Print<Printer::Color::RED>(
				std::format("[ProjectManifest] Failed to prepare package environment: {}\n", package_environment.error()));
			return;
		}

	#ifdef _WIN32
		const char separator = ';';
	#else
		const char separator = ':';
	#endif
		for (const std::string& warning : package_environment->m_warnings)
		{
			Printer::Print<Printer::Color::YELLOW>(std::format("[ProjectManifest] {}\n", warning));
		}

		if (!package_environment->m_search_paths.empty())
		{
			SetEnvironmentVariable("MARMOT_PATH", JoinSearchPaths(package_environment->m_search_paths, separator));
		}
	}

	bool InitializeProject(const std::filesystem::path& target_dir, std::string_view project_name, std::string& error_message)
	{
		std::error_code ec;
		std::filesystem::path root = target_dir;
		if (root.empty())
		{
			root = std::filesystem::current_path(ec);
			if (ec)
			{
				error_message = "Failed to resolve current directory.";
				return false;
			}
		}

		if (!root.is_absolute())
		{
			root = std::filesystem::absolute(root, ec);
			if (ec)
			{
				error_message = "Failed to resolve project path.";
				return false;
			}
		}

		if (std::filesystem::exists(root, ec))
		{
			if (!std::filesystem::is_directory(root, ec))
			{
				error_message = std::format("Path is not a directory: {}", root.string());
				return false;
			}
		}
		else
		{
			if (!std::filesystem::create_directories(root, ec))
			{
				error_message = std::format("Failed to create directory: {}", root.string());
				return false;
			}
		}

		const std::filesystem::path manifest_path = root / ProjectManifestFileName;
		const std::filesystem::path src_dir = root / "src";
		const std::filesystem::path packages_dir = root / "packages";
		const std::filesystem::path test_dir = root / "test";
		const std::filesystem::path main_path = src_dir / "Main.mmt";

		if (std::filesystem::exists(manifest_path, ec))
		{
			error_message = std::format("project.marmot already exists at {}", manifest_path.string());
			return false;
		}

		if (std::filesystem::exists(main_path, ec))
		{
			error_message = std::format("Main module already exists at {}", main_path.string());
			return false;
		}

		if (!std::filesystem::create_directories(src_dir, ec))
		{
			if (ec)
			{
				error_message = std::format("Failed to create directory: {}", src_dir.string());
				return false;
			}
		}

		if (!std::filesystem::create_directories(packages_dir, ec))
		{
			if (ec)
			{
				error_message = std::format("Failed to create directory: {}", packages_dir.string());
				return false;
			}
		}

		if (!std::filesystem::create_directories(test_dir, ec))
		{
			if (ec)
			{
				error_message = std::format("Failed to create directory: {}", test_dir.string());
				return false;
			}
		}

		const std::string resolved_name = project_name.empty() ? DeriveProjectName(root) : std::string(project_name);
		const std::string escaped_name = EscapeTomlString(resolved_name);
		const std::string manifest_contents = std::format
		(
			"[project]\n"
			"name = \"{}\"\n"
			"entry = \"src/Main.mmt\"\n"
			"source_dir = \"src\"\n"
			"packages_dir = \"packages\"\n"
			"prelude_dir = \"MarmotPrelude\"\n"
			"\n"
			"[test]\n"
			"dir = \"test\"\n"
			"timeout_ms = 30000\n",
			escaped_name
		);

		if (!WriteFile(manifest_path, manifest_contents, error_message))
		{
			return false;
		}

		const std::string main_contents =
			"module Main\n"
			"\n"
			"def main = fn() -> Int => 0;\n";

		if (!WriteFile(main_path, main_contents, error_message))
		{
			return false;
		}

		return true;
	}

	bool AddDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string_view constraint, std::string& error_message)
	{
		if (package_name.empty())
		{
			error_message = "Package name cannot be empty.";
			return false;
		}

		const std::expected<MidoriVersion::VersionConstraint, std::string> parsed_constraint =
			MidoriVersion::VersionConstraint::Parse(constraint);
		if (!parsed_constraint.has_value())
		{
			error_message = std::format("Invalid version constraint '{}': {}", constraint, parsed_constraint.error());
			return false;
		}

		const std::optional<std::filesystem::path> manifest_path = ResolveManifestPathForEditing(input_path, error_message);
		if (!manifest_path.has_value())
		{
			return false;
		}

		try
		{
			toml::value document = toml::parse(*manifest_path);
			if (!document.is_table())
			{
				error_message = std::format("Manifest is not a TOML table: {}", manifest_path->string());
				return false;
			}

			toml::table& root = document.as_table();
			if (!root.contains("dependencies"))
			{
				root["dependencies"] = toml::value(toml::table{});
			}

			toml::value& dependencies_value = root["dependencies"];
			if (!dependencies_value.is_table())
			{
				error_message = std::format("Manifest dependencies table is not a table: {}", manifest_path->string());
				return false;
			}

			dependencies_value[std::string(package_name)] = std::string(constraint);
			return WriteTomlDocument(*manifest_path, document, error_message);
		}
		catch (const std::exception& e)
		{
			error_message = std::format("Failed to update {}: {}", manifest_path->string(), e.what());
			return false;
		}
	}

	bool RemoveDependency(const std::filesystem::path& input_path, std::string_view package_name, std::string& error_message)
	{
		if (package_name.empty())
		{
			error_message = "Package name cannot be empty.";
			return false;
		}

		const std::optional<std::filesystem::path> manifest_path = ResolveManifestPathForEditing(input_path, error_message);
		if (!manifest_path.has_value())
		{
			return false;
		}

		try
		{
			toml::value document = toml::parse(*manifest_path);
			if (!document.is_table())
			{
				error_message = std::format("Manifest is not a TOML table: {}", manifest_path->string());
				return false;
			}

			toml::table& root = document.as_table();
			if (!root.contains("dependencies") || !root.at("dependencies").is_table())
			{
				error_message = std::format("No [dependencies] table found in {}", manifest_path->string());
				return false;
			}

			toml::table& dependencies = root["dependencies"].as_table();
			if (dependencies.erase(std::string(package_name)) == 0u)
			{
				error_message = std::format("Dependency '{}' is not present in {}", package_name, manifest_path->string());
				return false;
			}

			if (dependencies.empty())
			{
				root.erase("dependencies");
			}

			return WriteTomlDocument(*manifest_path, document, error_message);
		}
		catch (const std::exception& e)
		{
			error_message = std::format("Failed to update {}: {}", manifest_path->string(), e.what());
			return false;
		}
	}
}

namespace MidoriPackage
{
	bool InitializePackage(const std::filesystem::path& target_dir, std::string_view package_name, std::string& error_message)
	{
		std::error_code ec;
		std::filesystem::path root = target_dir;
		if (root.empty())
		{
			root = std::filesystem::current_path(ec);
			if (ec)
			{
				error_message = "Failed to resolve current directory.";
				return false;
			}
		}

		if (!root.is_absolute())
		{
			root = std::filesystem::absolute(root, ec);
			if (ec)
			{
				error_message = "Failed to resolve package path.";
				return false;
			}
		}

		if (std::filesystem::exists(root, ec))
		{
			if (!std::filesystem::is_directory(root, ec))
			{
				error_message = std::format("Path is not a directory: {}", root.string());
				return false;
			}
		}
		else
		{
			if (!std::filesystem::create_directories(root, ec))
			{
				error_message = std::format("Failed to create directory: {}", root.string());
				return false;
			}
		}

		const std::filesystem::path manifest_path = root / PackageManifestFileName;
		if (std::filesystem::exists(manifest_path, ec))
		{
			error_message = std::format("package.marmot already exists at {}", manifest_path.string());
			return false;
		}

		const std::string resolved_name = package_name.empty() ? DerivePackageName(root) : std::string(package_name);
		const std::string module_name = SanitizeModuleName(resolved_name);
		const std::string module_file = module_name + ".mmt";
		const std::filesystem::path module_path = root / module_file;

		if (std::filesystem::exists(module_path, ec))
		{
			error_message = std::format("Package module already exists at {}", module_path.string());
			return false;
		}

		const std::string escaped_package_name = EscapeTomlString(resolved_name);
		const std::string escaped_module_file = EscapeTomlString(module_file);
		const std::string escaped_export = EscapeTomlString(module_name);
		const std::string manifest_contents = std::format
		(
			"[package]\n"
			"name = \"{}\"\n"
			"version = \"0.1.0\"\n"
			"authors = []\n"
			"description = \"\"\n"
			"license = \"MIT\"\n"
			"marmot_version = \">=1.0.0\"\n"
			"\n"
			"[package.modules]\n"
			"main = \"{}\"\n"
			"exports = [\"{}\"]\n",
			escaped_package_name,
			escaped_module_file,
			escaped_export
		);

		if (!WriteFile(manifest_path, manifest_contents, error_message))
		{
			return false;
		}

		const std::string module_contents = std::format
		(
			"module {}\n"
			"public export {{ Hello }}\n"
			"\n"
			"def Hello = fn() -> Text => \"Hello from {}\";\n",
			module_name,
			module_name
		);

		if (!WriteFile(module_path, module_contents, error_message))
		{
			return false;
		}

		return true;
	}
}
