#include "CompiledModule.h"
#include "Support/Optional.h"

#include <utility>

CompiledModule::CompiledModule(std::shared_ptr<const ModuleExportMetadata> metadata)
	: m_export_metadata(std::move(metadata)),
	m_bytecode(MarmotCompiler::None)
{
}

const std::string& CompiledModule::ModuleName() const
{
	return m_export_metadata->m_module_name;
}

const std::filesystem::path& CompiledModule::FilePath() const
{
	return m_export_metadata->m_file_path;
}

const CompiledModule::SymbolTable& CompiledModule::Symbols() const
{
	return m_export_metadata->m_symbols;
}

const std::vector<CompilerWarning>& CompiledModule::Warnings() const
{
	return m_warnings;
}

const MarmotCompiler::Optional<BytecodeModule>& CompiledModule::Bytecode() const &
{
	return m_bytecode;
}

BytecodeModule CompiledModule::TakeBytecode() &&
{
	BytecodeModule bytecode = std::move(m_bytecode).Value();
	m_bytecode.Reset();
	return bytecode;
}

const std::string& CompiledModule::MarmotIR() const
{
	return m_marmot_ir;
}

const std::string& CompiledModule::Ast() const
{
	return m_ast;
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

CompiledModule CompiledModule::WithMarmotIR(std::string marmot_ir) &&
{
	m_marmot_ir = std::move(marmot_ir);
	return std::move(*this);
}

CompiledModule CompiledModule::WithAst(std::string ast) &&
{
	m_ast = std::move(ast);
	return std::move(*this);
}

CompiledModule CompiledModule::WithDefinitions(ModuleDefinitions definitions) &&
{
	m_definitions.Emplace(std::move(definitions));
	return std::move(*this);
}

ModuleDefinitions CompiledModule::TakeDefinitions() &&
{
	return std::move(m_definitions).Value();
}
