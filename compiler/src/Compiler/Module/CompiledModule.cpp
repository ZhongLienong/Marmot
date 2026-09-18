#include "CompiledModule.h"

#include <utility>

CompiledModule::CompiledModule(std::string module_name, std::filesystem::path file_path, SymbolTable symbols, TypeEnvironment type_signatures, TypeclassMetadataMap typeclass_metadata)
	: m_module_name(std::move(module_name)),
	m_file_path(std::move(file_path)),
	m_symbols(std::move(symbols)),
	m_type_signatures(std::move(type_signatures)),
	m_typeclass_metadata(std::move(typeclass_metadata)),
	m_bytecode(std::nullopt)
{
}

const std::string& CompiledModule::ModuleName() const
{
	return m_module_name;
}

const std::filesystem::path& CompiledModule::FilePath() const
{
	return m_file_path;
}

const CompiledModule::SymbolTable& CompiledModule::Symbols() const
{
	return m_symbols;
}

const CompiledModule::TypeEnvironment& CompiledModule::TypeSignatures() const
{
	return m_type_signatures;
}

const CompiledModule::TypeclassMetadataMap& CompiledModule::TypeclassMetadataByName() const
{
	return m_typeclass_metadata;
}

const std::vector<CompilerWarning>& CompiledModule::Warnings() const
{
	return m_warnings;
}

const std::optional<BytecodeModule>& CompiledModule::Bytecode() const &
{
	return m_bytecode;
}

BytecodeModule CompiledModule::TakeBytecode() &&
{
	BytecodeModule bytecode = std::move(m_bytecode).value();
	m_bytecode.reset();
	return bytecode;
}

bool CompiledModule::SymbolTable::HasExport(std::string_view name) const
{
	return m_exports.contains(std::string(name));
}

VisibilityLevel CompiledModule::SymbolTable::GetExportVisibility(std::string_view name) const
{
	const VisibilityLevel* visibility = FindExportVisibility(name);
	if (visibility != nullptr)
	{
		return *visibility;
	}
	return VisibilityLevel::Internal;  // Default to internal if not found
}

const VisibilityLevel* CompiledModule::SymbolTable::FindExportVisibility(std::string_view name) const
{
	const std::string name_string(name);
	ExportVisibilityMap::const_iterator it = m_export_visibility.find(name_string);
	if (it == m_export_visibility.end())
	{
		return nullptr;
	}

	return std::addressof(it->second);
}

CompiledModule::SymbolTable CompiledModule::SymbolTable::WithExport(std::string name, VisibilityLevel visibility) const &
{
	return SymbolTable(*this).WithExport(std::move(name), visibility);
}

CompiledModule::SymbolTable CompiledModule::SymbolTable::WithExport(std::string name, VisibilityLevel visibility) &&
{
	m_exports.insert(name);
	m_export_visibility.insert_or_assign(name, visibility);
	return std::move(*this);
}

CompiledModule CompiledModule::WithSymbols(SymbolTable symbols) &&
{
	m_symbols = std::move(symbols);
	return std::move(*this);
}

CompiledModule CompiledModule::WithTypeSignatures(TypeEnvironment type_signatures) &&
{
	m_type_signatures = std::move(type_signatures);
	return std::move(*this);
}

CompiledModule CompiledModule::WithTypeclassMetadata(TypeclassMetadataMap typeclass_metadata) &&
{
	m_typeclass_metadata = std::move(typeclass_metadata);
	return std::move(*this);
}

CompiledModule CompiledModule::WithWarnings(std::vector<CompilerWarning> warnings) &&
{
	m_warnings = std::move(warnings);
	return std::move(*this);
}

CompiledModule CompiledModule::WithBytecode(BytecodeModule bytecode) &&
{
	m_bytecode = std::move(bytecode);
	return std::move(*this);
}
