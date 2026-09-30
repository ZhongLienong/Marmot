#include "BytecodeModule.h"

#include <utility>

BytecodeModule::SourceProvenance::SourceProvenance(int line, std::optional<int> column, std::optional<size_t> caret_length, std::optional<std::string> source_line)
	: m_line(line),
	m_column(column),
	m_caret_length(caret_length),
	m_source_line(std::move(source_line))
{
}

BytecodeModule::ExportedSymbol::ExportedSymbol(std::string name, size_t proc_index, size_t global_index, SymbolType type, std::optional<SourceProvenance> source_provenance)
	: m_procedure_index(proc_index),
	m_global_index(global_index),
	m_type(type),
	m_name(std::move(name)),
	m_source_provenance(std::move(source_provenance))
{
}

BytecodeModule::ExportedSymbol::ExportedSymbol(std::string name, size_t index, SymbolType type, std::optional<SourceProvenance> source_provenance)
	: ExportedSymbol(std::move(name), index, index, type, std::move(source_provenance))
{
}

BytecodeModule::ImportedSymbol::ImportedSymbol(std::string name, std::string from_module, std::optional<SourceProvenance> source_provenance)
	: m_name(std::move(name)),
	m_from_module(std::move(from_module)),
	m_source_provenance(std::move(source_provenance))
{
}

BytecodeModule::BytecodeModule(std::string module_name, std::filesystem::path source_path)
	: m_module_name(std::move(module_name)),
	m_source_path(std::move(source_path))
{
}

BytecodeModule BytecodeModule::WithProcedure(BytecodeStream procedure) &&
{
	m_procedures.push_back(std::move(procedure));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithExport(ExportedSymbol export_symbol) &&
{
	m_exports.push_back(std::move(export_symbol));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithImport(ImportedSymbol import_symbol) &&
{
	m_imports.push_back(std::move(import_symbol));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithString(std::string str) &&
{
	m_string_pool.push_back(std::move(str));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithProcedureName(std::string name) &&
{
	m_procedure_names.push_back(std::move(name));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithGlobalVariable(std::string variable) &&
{
	m_global_variables.push_back(std::move(variable));
	return std::move(*this);
}

BytecodeModule BytecodeModule::WithGenericFunction(std::string name, GenericFunctionInfo info) &&
{
	m_generic_functions.emplace(std::move(name), std::move(info));
	return std::move(*this);
}
