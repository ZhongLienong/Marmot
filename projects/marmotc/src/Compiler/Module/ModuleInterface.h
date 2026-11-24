#pragma once

#include "Compiler/Module/Module.h"
#include "Compiler/Module/TypeclassMetadata.h"
#include "Compiler/Module/GenericFunctionInfo.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

struct ModuleInterface
{
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MarmotType>>;
	using ExportSet = std::unordered_set<std::string>;
	using ExportVisibilityMap = std::unordered_map<std::string, VisibilityLevel>;

	struct SymbolTable
	{
		[[nodiscard]] const VisibilityLevel* FindExportVisibility(std::string_view name) const;

		bool HasExport(std::string_view name) const;

		VisibilityLevel GetExportVisibility(std::string_view name) const;

		[[nodiscard]] SymbolTable WithExport(std::string name, VisibilityLevel visibility) const &;

		[[nodiscard]] SymbolTable WithExport(std::string name, VisibilityLevel visibility) &&;

	private:
		ExportSet m_exports;
		ExportVisibilityMap m_export_visibility;
	};

	std::string m_module_name;
	std::filesystem::path m_file_path;
	SymbolTable m_symbols;
	TypeEnvironment m_type_signatures;
	TypeclassMetadataMap m_typeclass_metadata;
	GenericFunctions m_generic_functions;

	ModuleInterface(std::string module_name, std::filesystem::path file_path, SymbolTable symbols, TypeEnvironment type_signatures, TypeclassMetadataMap typeclass_metadata, GenericFunctions generic_functions);
};
