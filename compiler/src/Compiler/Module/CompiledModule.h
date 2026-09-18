#pragma once

#include "Common/Error/Error.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Module/Module.h"
#include <string>
#include <filesystem>
#include <unordered_set>
#include <unordered_map>
#include <string_view>
#include <optional>
#include <memory>

struct CompiledModule
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
		std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>> m_instance_associated_type_bindings;
	};
	using TypeclassMethodMap = std::unordered_map<std::string, std::unordered_set<std::string>>;
	using TypeclassInstanceMap = std::unordered_map<std::string, std::vector<std::string>>;
	using TypeclassMetadataMap = std::unordered_map<std::string, TypeclassMetadata>;

	CompiledModule(std::string module_name, std::filesystem::path file_path, SymbolTable symbols, TypeEnvironment type_signatures = {}, TypeclassMetadataMap typeclass_metadata = {});

	CompiledModule(const CompiledModule&) = delete;
	CompiledModule& operator=(const CompiledModule&) = delete;
	CompiledModule(CompiledModule&&) noexcept = default;
	CompiledModule& operator=(CompiledModule&&) noexcept = default;

	[[nodiscard]] const std::string& ModuleName() const;

	[[nodiscard]] const std::filesystem::path& FilePath() const;

	[[nodiscard]] const SymbolTable& Symbols() const;

	[[nodiscard]] const TypeEnvironment& TypeSignatures() const;

	[[nodiscard]] const TypeclassMetadataMap& TypeclassMetadataByName() const;

	[[nodiscard]] const std::vector<CompilerWarning>& Warnings() const;

	[[nodiscard]] const std::optional<BytecodeModule>& Bytecode() const &;

	[[nodiscard]] BytecodeModule TakeBytecode() &&;

	[[nodiscard]] CompiledModule WithSymbols(SymbolTable symbols) &&;

	[[nodiscard]] CompiledModule WithTypeSignatures(TypeEnvironment type_signatures) &&;

	[[nodiscard]] CompiledModule WithTypeclassMetadata(TypeclassMetadataMap typeclass_metadata) &&;

	[[nodiscard]] CompiledModule WithWarnings(std::vector<CompilerWarning> warnings) &&;

	[[nodiscard]] CompiledModule WithBytecode(BytecodeModule bytecode) &&;

private:
	std::string m_module_name;
	std::filesystem::path m_file_path;
	SymbolTable m_symbols;
	TypeEnvironment m_type_signatures;
	TypeclassMetadataMap m_typeclass_metadata;
	std::vector<CompilerWarning> m_warnings;
	std::optional<BytecodeModule> m_bytecode;        // Per-module bytecode for incremental compilation
};
