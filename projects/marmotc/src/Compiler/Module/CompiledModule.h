#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Module/ModuleInterface.h"
#include <string>
#include <filesystem>
#include <vector>
#include <optional>
#include <memory>

struct CompiledModule
{
	using TypeEnvironment = ModuleInterface::TypeEnvironment;
	using ExportSet = ModuleInterface::ExportSet;
	using ExportVisibilityMap = ModuleInterface::ExportVisibilityMap;
	using SymbolTable = ModuleInterface::SymbolTable;
	using TypeclassMetadata = ModuleInterface::TypeclassMetadata;
	using TypeclassMethodMap = ModuleInterface::TypeclassMethodMap;
	using TypeclassInstanceMap = ModuleInterface::TypeclassInstanceMap;
	using TypeclassMetadataMap = ModuleInterface::TypeclassMetadataMap;

	explicit CompiledModule(std::shared_ptr<const ModuleInterface> interface);

	CompiledModule(const CompiledModule&) = delete;
	CompiledModule& operator=(const CompiledModule&) = delete;
	CompiledModule(CompiledModule&&) noexcept = default;
	CompiledModule& operator=(CompiledModule&&) noexcept = default;

	[[nodiscard]] const std::string& ModuleName() const;

	[[nodiscard]] const std::filesystem::path& FilePath() const;

	[[nodiscard]] const SymbolTable& Symbols() const;

	[[nodiscard]] const TypeEnvironment& TypeSignatures() const;

	[[nodiscard]] const std::vector<CompilerWarning>& Warnings() const;

	[[nodiscard]] const std::optional<BytecodeModule>& Bytecode() const &;

	[[nodiscard]] BytecodeModule TakeBytecode() &&;

	// The module's MidoriIR as text, when --emit-ir asked for it; empty otherwise.
	[[nodiscard]] const std::string& MidoriIR() const;

	// The module's checked syntax tree as text, when --emit-ast asked for it; empty otherwise.
	[[nodiscard]] const std::string& Ast() const;

	[[nodiscard]] CompiledModule WithWarnings(std::vector<CompilerWarning> warnings) &&;

	[[nodiscard]] CompiledModule WithBytecode(BytecodeModule bytecode) &&;

	[[nodiscard]] CompiledModule WithMidoriIR(std::string midori_ir) &&;

	[[nodiscard]] CompiledModule WithAst(std::string ast) &&;

private:
	std::shared_ptr<const ModuleInterface> m_interface;
	std::vector<CompilerWarning> m_warnings;
	std::optional<BytecodeModule> m_bytecode;        // Per-module bytecode for incremental compilation
	std::string m_midori_ir;
	std::string m_ast;
};
