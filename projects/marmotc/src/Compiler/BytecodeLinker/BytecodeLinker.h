#pragma once

#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Result/Result.h"

#include <expected>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace MarmotResult
{
	using BytecodeLinkerResult = Result<MarmotExecutable>;
}

class BytecodeLinker
{
private:
	struct LinkedExport
	{
		size_t m_global_procedure_index = 0u;
		const BytecodeModule* m_module = nullptr;
		const BytecodeModule::ExportedSymbol* m_export = nullptr;
	};

	std::vector<BytecodeModule> m_modules;
	std::string m_entry_module_name;
	std::unordered_map<std::string, LinkedExport> m_global_symbol_table;
	// An import that resolves to nothing used to fall back to global 0, so a
	// missing symbol surfaced as a memory access violation at run time instead of
	// as a message. Failures are recorded here and reported after concatenation.
	mutable std::vector<std::string> m_unresolved_imports;
	std::unordered_map<std::string, size_t> m_module_base_procedure_indices;
	std::unordered_map<std::string, size_t> m_module_base_global_indices;
	std::unordered_map<std::string, std::vector<size_t>> m_module_string_index_mappings;
	std::vector<std::string> m_global_string_pool;
	std::vector<std::string> m_global_procedure_names;
	std::vector<std::string> m_global_procedure_source_paths;
	std::vector<BytecodeStream> m_global_procedures;
	std::vector<std::string> m_global_variables;
	std::vector<bool> m_global_pointers;
	std::vector<Shape> m_global_shapes{ Shape{}, Shape{ 0u } };
	std::map<Shape, uint16_t> m_global_shape_indices{ { Shape{}, SCALAR_SHAPE }, { Shape{ 0u }, FIRST_FIELD_SHAPE } };
	std::unordered_map<std::string, std::vector<uint16_t>> m_module_shape_mappings;
	MarmotExecutable::SourceFileTable m_global_source_files;

public:
	BytecodeLinker(std::vector<BytecodeModule>&& modules, std::string_view entry_module_name);

	MarmotResult::BytecodeLinkerResult Link();

private:
	void AssignModuleBaseOffsets();

	MarmotResult::VoidResult BuildGlobalSymbolTable();

	void MergeConstantPools();

	MarmotResult::VoidResult MergeShapes();

	void MergeFunctionNames();

	void MergeGlobalVariables();

	std::vector<NativeLibraryImport> MergeNativeLibraries() const;

	MarmotResult::VoidResult ResolveImportsAndPatch();

	void ConcatenateBytecode();

	void PopulateReferencedGlobals();

	BytecodeStream BuildBootstrapProcedure() const;

	const BytecodeModule* FindModule(const std::string& module_name) const;

	std::string MakeSymbolKey(const std::string& module_name, const std::string& symbol_name) const;


	size_t MergeString(const std::string& str);

	// A re-exported name is an alias, so the module that answers for it is the
	// one at the end of the chain.
	const BytecodeModule* DefiningModule(const std::string& module_name, const std::string& symbol_name) const;

	std::optional<size_t> FindSymbolInExports(const BytecodeModule& module, const std::string& symbol_name) const;

	std::optional<size_t> FindSymbolInGlobals(const BytecodeModule& module, const std::string& symbol_name, size_t base_global_offset) const;

	MarmotResult::VoidResult ValidateImport(const BytecodeModule& module, const BytecodeModule::ImportedSymbol& import) const;

	std::vector<size_t> ResolveImports(const BytecodeModule& module) const;

	void PatchProcedure(BytecodeStream& procedure, size_t module_proc_base_offset, size_t module_global_base_offset, const std::vector<size_t>& import_resolved_indices, const std::vector<size_t>& string_mapping, const std::vector<uint16_t>& shape_mapping) const;
};
