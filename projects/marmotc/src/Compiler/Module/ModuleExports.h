#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Module/ModuleImports.h"

struct LoweredModule;

struct PublishedModuleInterface
{
	std::shared_ptr<const ModuleInterface> m_interface;
	std::unordered_map<std::string, std::string> m_reexports;
};

class ModuleExports
{
public:
	using TypeEnvironment = ModuleInterface::TypeEnvironment;

	ModuleExports() = default;
	ModuleExports(const ModuleDeclaration* declaration, std::filesystem::path file_path, TypeEnvironment types, TypeclassMetadataMap typeclasses);

	const std::string& ModuleName() const;
	const ModuleInterface::ExportSet& Names() const;
	void RecordInferredTypes(const TypeEnvironment& inferred);
	std::expected<PublishedModuleInterface, CompilerError> Publish(LoweredModule& lowered, const ModuleImports& imports, std::span<const UseImport> use_imports, WorkerCrossings crossings, const std::vector<std::string>& source_lines) &&;

private:
	std::string m_module_name;
	std::filesystem::path m_file_path;
	ModuleDeclaration::ExportList m_declarations;
	ModuleInterface::SymbolTable m_symbols;
	ModuleInterface::ExportSet m_names;
	TypeEnvironment m_types;
	TypeclassMetadataMap m_typeclasses;
};
