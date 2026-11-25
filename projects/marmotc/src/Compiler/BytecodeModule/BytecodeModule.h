#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Bytecode/Executable/Executable.h"
#include "Support/Optional.h"

struct BytecodeModule
{
	enum class SymbolType : std::uint8_t
	{
		FUNCTION,
		FOREIGN_FUNCTION,
		GLOBAL_VARIABLE,
		STRUCT_TYPE,
		UNION_TYPE
	};

	struct SourceProvenance
	{
		int m_line = 0;
		MarmotCompiler::Optional<int> m_column = MarmotCompiler::None;
		MarmotCompiler::Optional<size_t> m_caret_length = MarmotCompiler::None;
		MarmotCompiler::Optional<std::string> m_source_line = MarmotCompiler::None;

		SourceProvenance() = default;
		SourceProvenance(int line, MarmotCompiler::Optional<int> column, MarmotCompiler::Optional<size_t> caret_length, MarmotCompiler::Optional<std::string> source_line);
	};

	struct ExportedSymbol
	{
		size_t m_procedure_index;  // Index into m_procedures for functions
		size_t m_global_index;     // Index into m_global_variables where the symbol is stored
		SymbolType m_type;
		std::string m_name;
		MarmotCompiler::Optional<SourceProvenance> m_source_provenance;

		ExportedSymbol(std::string name, size_t proc_index, size_t global_index, SymbolType type, MarmotCompiler::Optional<SourceProvenance> source_provenance = MarmotCompiler::None);

		ExportedSymbol(std::string name, size_t index, SymbolType type, MarmotCompiler::Optional<SourceProvenance> source_provenance = MarmotCompiler::None);
	};
	struct ImportedSymbol
	{
		std::string m_name;
		std::string m_from_module;
		MarmotCompiler::Optional<SourceProvenance> m_source_provenance;

		ImportedSymbol(std::string name, std::string from_module, MarmotCompiler::Optional<SourceProvenance> source_provenance = MarmotCompiler::None);
	};

	using ProcedureList = std::vector<BytecodeStream>;
	using ExportList = std::vector<ExportedSymbol>;
	using ImportList = std::vector<ImportedSymbol>;
	using StringPool = std::vector<std::string>;
	using ProcedureNameList = std::vector<std::string>;
	using GlobalVariableList = std::vector<std::string>;
	// An exported name this module does not define: the module it came from.
	using ReexportMap = std::unordered_map<std::string, std::string>;
	using SourceFileTable = MarmotExecutable::SourceFileTable;

	std::string m_module_name;
	std::filesystem::path m_source_path;
	ProcedureList m_procedures;
	ExportList m_exports;
	ImportList m_imports;
	StringPool m_string_pool;
	ProcedureNameList m_procedure_names;
	GlobalVariableList m_global_variables;
	// One per global, true where it holds a pointer.
	std::vector<bool> m_global_pointers;
	// The shapes this module's instructions and procedures name, by the index they
	// name; the linker merges them into the program's.
	std::vector<Shape> m_shapes;
	ReexportMap m_reexports;
	SourceFileTable m_source_files;
	std::vector<NativeLibraryImport> m_native_libraries;

	BytecodeModule() = default;
	BytecodeModule(std::string module_name, std::filesystem::path source_path);

	BytecodeModule(const BytecodeModule&) = delete;
	BytecodeModule& operator=(const BytecodeModule&) = delete;
	BytecodeModule(BytecodeModule&&) noexcept = default;
	BytecodeModule& operator=(BytecodeModule&&) noexcept = default;
};
