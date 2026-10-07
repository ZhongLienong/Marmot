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
