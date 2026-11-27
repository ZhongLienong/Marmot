#pragma once

#include "Compiler/Analysis/DefinitionAnalysis.h"
#include "Compiler/Error/CompilerError.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Module/ModuleExportMetadata.h"
#include "Support/Optional.h"
#include <string>
#include <filesystem>
#include <vector>
#include <memory>

struct CompiledModule
{
	using SymbolTable = ModuleExportMetadata::SymbolTable;

	explicit CompiledModule(std::shared_ptr<const ModuleExportMetadata> metadata);

	CompiledModule(const CompiledModule&) = delete;
	CompiledModule& operator=(const CompiledModule&) = delete;
	CompiledModule(CompiledModule&&) noexcept = default;
	CompiledModule& operator=(CompiledModule&&) noexcept = default;

	[[nodiscard]] const std::string& ModuleName() const;

	[[nodiscard]] const std::filesystem::path& FilePath() const;

	[[nodiscard]] const SymbolTable& Symbols() const;

	[[nodiscard]] const std::vector<CompilerWarning>& Warnings() const;

	[[nodiscard]] const MarmotCompiler::Optional<BytecodeModule>& Bytecode() const &;

	[[nodiscard]] BytecodeModule TakeBytecode() &&;

	// The module's MarmotIR as text, when --emit-ir asked for it; empty otherwise.
	[[nodiscard]] const std::string& MarmotIR() const;

	// The module's checked syntax tree as text, when --emit-ast asked for it; empty otherwise.
	[[nodiscard]] const std::string& Ast() const;

	[[nodiscard]] CompiledModule WithWarnings(std::vector<CompilerWarning> warnings) &&;

	[[nodiscard]] CompiledModule WithBytecode(BytecodeModule bytecode) &&;

	[[nodiscard]] CompiledModule WithMarmotIR(std::string marmot_ir) &&;

	[[nodiscard]] CompiledModule WithAst(std::string ast) &&;

	[[nodiscard]] CompiledModule WithDefinitions(ModuleDefinitions definitions) &&;

	[[nodiscard]] ModuleDefinitions TakeDefinitions() &&;

private:
	std::shared_ptr<const ModuleExportMetadata> m_export_metadata;
	std::vector<CompilerWarning> m_warnings;
	MarmotCompiler::Optional<BytecodeModule> m_bytecode;        // Per-module bytecode for incremental compilation
	std::string m_marmot_ir;
	std::string m_ast;
	MarmotCompiler::Optional<ModuleDefinitions> m_definitions;
};
