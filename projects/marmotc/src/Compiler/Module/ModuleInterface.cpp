#include "ModuleInterface.h"

#include <utility>

ModuleInterface::ModuleInterface(std::string module_name, std::filesystem::path file_path, SymbolTable symbols, TypeEnvironment type_signatures, TypeclassMetadataMap typeclass_metadata, GenericFunctionTable::Functions generic_functions)
	: m_module_name(std::move(module_name)),
	m_file_path(std::move(file_path)),
	m_symbols(std::move(symbols)),
	m_type_signatures(std::move(type_signatures)),
	m_typeclass_metadata(std::move(typeclass_metadata)),
	m_generic_functions(std::move(generic_functions))
{
}

bool ModuleInterface::SymbolTable::HasExport(std::string_view name) const
{
	return m_exports.contains(std::string(name));
}

VisibilityLevel ModuleInterface::SymbolTable::GetExportVisibility(std::string_view name) const
{
	const VisibilityLevel* visibility = FindExportVisibility(name);
	if (visibility != nullptr)
	{
		return *visibility;
	}
	return VisibilityLevel::Internal;  // Default to internal if not found
}

const VisibilityLevel* ModuleInterface::SymbolTable::FindExportVisibility(std::string_view name) const
{
	const std::string name_string(name);
	ExportVisibilityMap::const_iterator it = m_export_visibility.find(name_string);
	if (it == m_export_visibility.end())
	{
		return nullptr;
	}

	return std::addressof(it->second);
}

ModuleInterface::SymbolTable ModuleInterface::SymbolTable::WithExport(std::string name, VisibilityLevel visibility) const &
{
	return SymbolTable(*this).WithExport(std::move(name), visibility);
}

ModuleInterface::SymbolTable ModuleInterface::SymbolTable::WithExport(std::string name, VisibilityLevel visibility) &&
{
	m_exports.insert(name);
	m_export_visibility.insert_or_assign(name, visibility);
	return std::move(*this);
}
