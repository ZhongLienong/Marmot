#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"
#include "Compiler/Token/Token.h"
#include "support/OutputCapture.h"
#include "support/SourceFixture.h"

#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace MarmotTest
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
		MarmotProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;
		std::optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;

		ParsedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports);
	};

	struct TypedSnippet
	{
		SourceFixture m_source;
		MarmotProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;

		TypedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings);
	};

	struct AnalyzedSnippet
	{
		SourceFixture m_source;
		MarmotProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;

		AnalyzedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings);
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

	[[nodiscard]] std::expected<TypedSnippet, MarmotResult::CompilerDiagnostics> TypeCheckSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<TypedSnippet, CompilerError> TypeCheckSnippet(std::string source_code, std::string file_name = "Test.mmt");

	// Lowers one module to MarmotIR, as the compiler's lowering stage does.
	[[nodiscard]] std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> LowerSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	// Lowers one module, optimizes its MarmotIR and emits it, as the compiler
	// does after static analysis.
	[[nodiscard]] std::expected<BytecodeModule, MarmotResult::CompilerDiagnostics> GenerateBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::expected<AnalyzedSnippet, CompilerError> AnalyzeSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] MarmotResult::CompilationResult CompileSnippetWithReport(std::string source_code, std::string file_name = "Test.mmt");
	[[nodiscard]] const MarmotResult::CompilerReport& CompilationReport(const MarmotResult::CompilationResult& compilation_result);

	[[nodiscard]] MarmotResult::CompilerResult CompileSnippet(std::string source_code, std::string file_name = "Test.mmt");
	[[nodiscard]] std::pair<int, CapturedOutput> RunExecutable(const MarmotExecutable& executable, const std::vector<std::string>& arguments = {});

	[[nodiscard]] std::expected<ExecutedSnippet, CompilerError> ExecuteSnippet(std::string source_code, std::string file_name = "Test.mmt");

	[[nodiscard]] std::vector<Token::Name> CollectTokenNames(const TokenStream& tokens, bool include_whitespace = false, bool include_end_of_file = false);
}
