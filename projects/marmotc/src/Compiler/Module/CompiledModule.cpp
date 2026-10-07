#include "CompiledModule.h"

#include <utility>

CompiledModule::CompiledModule(std::shared_ptr<const ModuleInterface> interface)
	: m_interface(std::move(interface)),
	m_bytecode(std::nullopt)
{
}

const std::string& CompiledModule::ModuleName() const
{
	return m_interface->m_module_name;
}

const std::filesystem::path& CompiledModule::FilePath() const
{
	return m_interface->m_file_path;
}

const CompiledModule::SymbolTable& CompiledModule::Symbols() const
{
	return m_interface->m_symbols;
}

const CompiledModule::TypeEnvironment& CompiledModule::TypeSignatures() const
{
	return m_interface->m_type_signatures;
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

const std::string& CompiledModule::MidoriIR() const
{
	return m_midori_ir;
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

CompiledModule CompiledModule::WithMidoriIR(std::string midori_ir) &&
{
	m_midori_ir = std::move(midori_ir);
	return std::move(*this);
}

CompiledModule CompiledModule::WithAst(std::string ast) &&
{
	m_ast = std::move(ast);
	return std::move(*this);
}
