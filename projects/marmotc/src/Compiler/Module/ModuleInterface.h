#pragma once

#include "Compiler/Module/Module.h"
#include "Compiler/Lowering/GenericFunctionTable.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

struct ModuleInterface
{
	using TypeEnvironment = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
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

	struct TypeclassMetadata
	{
		std::unordered_map<std::string, std::shared_ptr<MidoriType>> m_method_types;
		std::unordered_set<std::string> m_method_names;
		std::vector<std::string> m_type_param_names;
		std::vector<std::string> m_associated_type_names;
		std::vector<std::string> m_instance_methods;  // Mangled instance method names (e.g., show_Show_Int)
		std::vector<std::vector<std::shared_ptr<MidoriType>>> m_instance_type_args;
		// Of those, the ones this module declares itself.
		std::vector<std::vector<std::shared_ptr<MidoriType>>> m_declared_instance_type_args;
		std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>> m_instance_associated_type_bindings;
	};
	using TypeclassMethodMap = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using TypeclassInstanceMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassMetadataMap = std::unordered_map<std::string, TypeclassMetadata>;

	std::string m_module_name;
	std::filesystem::path m_file_path;
	SymbolTable m_symbols;
	TypeEnvironment m_type_signatures;
	TypeclassMetadataMap m_typeclass_metadata;
	GenericFunctionTable::Functions m_generic_functions;

	ModuleInterface(std::string module_name, std::filesystem::path file_path, SymbolTable symbols, TypeEnvironment type_signatures, TypeclassMetadataMap typeclass_metadata, GenericFunctionTable::Functions generic_functions);
};
