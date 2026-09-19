#pragma once

#include "Common/Error/Error.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"
#include "Compiler/Token/Token.h"
#include "support/OutputCapture.h"
#include "support/SourceFixture.h"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace MidoriTest
{
	struct LexedSnippet
	{
		SourceFixture m_source;
		TokenStream m_tokens;

		LexedSnippet(SourceFixture source, TokenStream&& tokens);
	};

	struct ParsedSnippet
	{
		SourceFixture m_source;
		MidoriProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;
		std::optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;

		ParsedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports);
	};

	struct TypedSnippet
	{
		SourceFixture m_source;
		MidoriProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;

		TypedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings);
	};

	struct AnalyzedSnippet
	{
		SourceFixture m_source;
		MidoriProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;
		std::vector<CompilerError> m_errors;

		AnalyzedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::vector<CompilerError>&& errors);
	};

	struct ExecutedSnippet
	{
		SourceFixture m_source;
		int m_exit_code = 0;
		CapturedOutput m_output;

		ExecutedSnippet(SourceFixture source, int exit_code, CapturedOutput output);
	};

	[[nodiscard]] std::expected<LexedSnippet, CompilerError> LexSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<ParsedSnippet, CompilerError> ParseSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<TypedSnippet, MidoriResult::CompilerDiagnostics> TypeCheckSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<TypedSnippet, CompilerError> TypeCheckSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> GenerateBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	// Identical to GenerateBytecodeSnippetWithDiagnostics, except it runs
	// OptimizerManager between type checking and code generation, the way the
	// real compiler does (see Compiler.cpp's WithOptimizedAst). Use this when a
	// test needs to know what the optimizer pipeline does to codegen output -
	// GenerateBytecodeSnippetWithDiagnostics alone cannot detect a regression
	// that only shows up after optimisation, because it never runs one.
	[[nodiscard]] std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> GenerateOptimizedBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<AnalyzedSnippet, CompilerError> AnalyzeSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] MidoriResult::CompilationResult CompileSnippetWithReport(std::string source_code, std::string file_name = "Test.mmt");
	[[nodiscard]] const MidoriResult::CompilerReport& CompilationReport(const MidoriResult::CompilationResult& compilation_result);

	[[nodiscard]] MidoriResult::CompilerResult CompileSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<ExecutedSnippet, CompilerError> ExecuteSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::vector<Token::Name> CollectTokenNames(const TokenStream& tokens, bool include_whitespace = false, bool include_end_of_file = false);
}
