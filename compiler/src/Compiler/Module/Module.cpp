#include "Module.h"

#include <algorithm>
#include <utility>

ModuleExport::ModuleExport(std::string_view symbol_name, VisibilityLevel visibility)
	: m_symbol_name(symbol_name),
	m_visibility(visibility)
{
}

UseImport::UseImport(std::string_view module_name, std::string_view symbol_name)
	: m_module_name(module_name),
	m_symbol_name(symbol_name)
{
}

ModuleDeclaration::ModuleDeclaration()
	: m_module_name(""),
	m_file_path(""),
	m_exports(),
	m_imports(),
	m_has_module_declaration(false)
{
}

ModuleDeclaration::ModuleDeclaration(std::string_view module_name, const std::filesystem::path& file_path)
	: m_module_name(module_name),
	m_file_path(file_path),
	m_exports(),
	m_imports(),
	m_has_module_declaration(true)
{
}

const std::string& ModuleDeclaration::ModuleName() const
{
	return m_module_name;
}

const std::filesystem::path& ModuleDeclaration::FilePath() const
{
	return m_file_path;
}

const ModuleDeclaration::ExportList& ModuleDeclaration::Exports() const
{
	return m_exports;
}

const ModuleDeclaration::ImportList& ModuleDeclaration::Imports() const
{
	return m_imports;
}

bool ModuleDeclaration::HasModuleDeclaration() const
{
	return m_has_module_declaration;
}

bool ModuleDeclaration::HasExport(std::string_view symbol_name) const
{
	return FindExport(symbol_name) != nullptr;
}

VisibilityLevel ModuleDeclaration::GetExportVisibility(std::string_view symbol_name) const
{
	const ModuleExport* export_entry = FindExport(symbol_name);
	if (export_entry != nullptr)
	{
		return export_entry->m_visibility;
	}

	return VisibilityLevel::Internal;
}

const ModuleExport* ModuleDeclaration::FindExport(std::string_view symbol_name) const
{
	std::vector<ModuleExport>::const_iterator it = std::ranges::find_if
	(
		m_exports,
		[&symbol_name](const ModuleExport& exp)
		{
			return exp.m_symbol_name == symbol_name;
		}
	);

	if (it == m_exports.cend())
	{
		return nullptr;
	}

	return std::addressof(*it);
}

ModuleDeclaration ModuleDeclaration::WithExport(ModuleExport export_entry) const &
{
	return ModuleDeclaration(*this).WithExport(std::move(export_entry));
}

ModuleDeclaration ModuleDeclaration::WithExport(ModuleExport export_entry) &&
{
	m_exports.push_back(std::move(export_entry));
	return std::move(*this);
}

ModuleDeclaration ModuleDeclaration::WithExports(ModuleDeclaration::ExportList exports) const &
{
	return ModuleDeclaration(*this).WithExports(std::move(exports));
}

ModuleDeclaration ModuleDeclaration::WithExports(ModuleDeclaration::ExportList exports) &&
{
	m_exports = std::move(exports);
	return std::move(*this);
}

ModuleDeclaration ModuleDeclaration::WithImport(std::string import_name) const &
{
	return ModuleDeclaration(*this).WithImport(std::move(import_name));
}

ModuleDeclaration ModuleDeclaration::WithImport(std::string import_name) &&
{
	m_imports.push_back(std::move(import_name));
	return std::move(*this);
}

ModuleDeclaration ModuleDeclaration::WithImports(ModuleDeclaration::ImportList imports) const &
{
	return ModuleDeclaration(*this).WithImports(std::move(imports));
}

ModuleDeclaration ModuleDeclaration::WithImports(ModuleDeclaration::ImportList imports) &&
{
	m_imports = std::move(imports);
	return std::move(*this);
}

ModuleDeclaration ModuleDeclaration::WithHasModuleDeclaration(bool has_module_declaration) const &
{
	return ModuleDeclaration(*this).WithHasModuleDeclaration(has_module_declaration);
}

ModuleDeclaration ModuleDeclaration::WithHasModuleDeclaration(bool has_module_declaration) &&
{
	m_has_module_declaration = has_module_declaration;
	return std::move(*this);
}
