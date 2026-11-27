#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Module/ModuleImports.h"
#include "Support/Expected.h"

struct LoweredModule;

struct PublishedModuleExports
{
	std::shared_ptr<const ModuleExportMetadata> m_export_metadata;
	std::unordered_map<std::string, std::string> m_reexports;
};

class ModuleExports
{
public:
	using TypeEnvironment = ModuleExportMetadata::TypeEnvironment;

	ModuleExports() = default;
	ModuleExports(const ModuleDeclaration* declaration, std::filesystem::path file_path, TypeEnvironment types, TypeclassMetadataMap typeclasses);

	const std::string& ModuleName() const;
	const ModuleExportMetadata::ExportSet& Names() const;
	void RecordInferredTypes(const TypeEnvironment& inferred);
	MarmotCompiler::Expected<PublishedModuleExports, CompilerError> Publish(LoweredModule& lowered, const ModuleImports& imports, std::span<const UseImport> use_imports, const std::vector<std::string>& source_lines) &&;

private:
	std::string m_module_name;
	std::filesystem::path m_file_path;
	ModuleDeclaration::ExportList m_declarations;
	ModuleExportMetadata::SymbolTable m_symbols;
	ModuleExportMetadata::ExportSet m_names;
	TypeEnvironment m_types;
	TypeclassMetadataMap m_typeclasses;
};
