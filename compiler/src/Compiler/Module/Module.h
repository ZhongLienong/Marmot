#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Compiler/AbstractSyntaxTree/Type.h"

enum class VisibilityLevel : std::uint8_t
{
	Public,      // Exported publicly (accessible to all importers)
	Private,     // Exported privately (accessible only to same namespace modules)
	Internal     // Not exported (module-internal only)
};

struct ModuleExport
{
	std::string m_symbol_name;
	VisibilityLevel m_visibility;

	ModuleExport(std::string_view symbol_name, VisibilityLevel visibility);
};

// Represents a symbol imported via 'use' statement
// e.g., "use IO.{Print, PrintLine}" creates UseImport entries for Print and PrintLine
struct UseImport
{
	std::string m_module_name;      // The module name (e.g., "IO")
	std::string m_symbol_name;      // The symbol name (e.g., "Print")

	UseImport(std::string_view module_name, std::string_view symbol_name);
};

struct ModuleDeclaration
{
	using ExportList = std::vector<ModuleExport>;
	using ImportList = std::vector<std::string>;

	ModuleDeclaration();

	ModuleDeclaration(std::string_view module_name, const std::filesystem::path& file_path);

	[[nodiscard]] const std::string& ModuleName() const;

	[[nodiscard]] const std::filesystem::path& FilePath() const;

	[[nodiscard]] const ExportList& Exports() const;

	[[nodiscard]] const ImportList& Imports() const;

	[[nodiscard]] bool HasModuleDeclaration() const;

	[[nodiscard]] const ModuleExport* FindExport(std::string_view symbol_name) const;

	bool HasExport(std::string_view symbol_name) const;

	VisibilityLevel GetExportVisibility(std::string_view symbol_name) const;

	[[nodiscard]] ModuleDeclaration WithExport(ModuleExport export_entry) const &;

	[[nodiscard]] ModuleDeclaration WithExport(ModuleExport export_entry) &&;

	[[nodiscard]] ModuleDeclaration WithExports(ExportList exports) const &;

	[[nodiscard]] ModuleDeclaration WithExports(ExportList exports) &&;

	[[nodiscard]] ModuleDeclaration WithImport(std::string import_name) const &;

	[[nodiscard]] ModuleDeclaration WithImport(std::string import_name) &&;

	[[nodiscard]] ModuleDeclaration WithImports(ImportList imports) const &;

	[[nodiscard]] ModuleDeclaration WithImports(ImportList imports) &&;

	[[nodiscard]] ModuleDeclaration WithHasModuleDeclaration(bool has_module_declaration) const &;

	[[nodiscard]] ModuleDeclaration WithHasModuleDeclaration(bool has_module_declaration) &&;

private:
	std::string m_module_name;
	std::filesystem::path m_file_path;
	ExportList m_exports;
	ImportList m_imports;
	bool m_has_module_declaration;          // True if file has explicit "module X" declaration
};
