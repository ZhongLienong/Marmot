#pragma once

#include "Compiler/Module/ModuleInterface.h"

#include <expected>
#include <span>

enum class ImportedSymbolAccess
{
	Accessible,
	ModuleNotFound,
	SymbolNotExported,
	PrivateInaccessible
};

struct ModuleImportConflict
{
	std::filesystem::path m_dependency;
	std::string m_message;

	ModuleImportConflict(std::filesystem::path dependency, std::string message);
};

class ModuleImports
{
public:
	using TypeEnvironment = ModuleInterface::TypeEnvironment;
	using ModuleTypes = std::unordered_map<std::string, TypeEnvironment>;

	static std::expected<ModuleImports, ModuleImportConflict> Collect(std::span<const std::shared_ptr<const ModuleInterface>> dependencies);

	bool ContainsModule(std::string_view module_name) const;
	ImportedSymbolAccess AccessFrom(std::string_view importer, const std::string& module_name, const std::string& symbol_name) const;
	std::vector<std::string> ModulesExporting(std::string_view importer, const std::string& symbol_name) const;
	std::vector<std::string> ReexportOrigins(std::string_view importer, const std::string& symbol_name, std::span<const UseImport> use_imports) const;
	void Reexport(const std::string& symbol_name, const std::string& origin, TypeEnvironment& types, GenericFunctions& generics) const;

	const ModuleTypes& TypesByModule() const;
	const TypeEnvironment& Types() const;
	const TypeclassMetadataMap& Typeclasses() const;
	const GenericFunctions& Generics() const;

private:
	std::unordered_map<std::string, ModuleInterface::SymbolTable> m_symbols;
	ModuleTypes m_types_by_module;
	TypeEnvironment m_types;
	TypeclassMetadataMap m_typeclasses;
	GenericFunctions m_generics;
};
