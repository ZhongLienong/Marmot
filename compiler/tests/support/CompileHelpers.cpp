#include "support/CompileHelpers.h"

#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/CodeGenerator/CodeGenerator.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/OptimizerManager/OptimizerManager.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"
#include "Utility/Driver/MidoriDriver.h"

#include <filesystem>
#include <print>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	struct PreparedModule
	{
		MidoriTest::SourceFixture m_source;
		TokenStream m_tokens;
		std::optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;

		PreparedModule(MidoriTest::SourceFixture source, TokenStream&& tokens, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
			: m_source(std::move(source)),
			m_tokens(std::move(tokens)),
			m_module_declaration(std::move(module_declaration)),
			m_use_imports(std::move(use_imports))
		{
		}
	};

	struct PreparedTypedModule
	{
		MidoriTest::SourceFixture m_source;
		MidoriProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;
		std::optional<ModuleDeclaration> m_module_declaration;

		PreparedTypedModule(MidoriTest::SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration)
			: m_source(std::move(source)),
			m_program(std::move(program)),
			m_warnings(std::move(warnings)),
			m_module_declaration(std::move(module_declaration))
		{
		}
	};

	std::expected<PreparedModule, CompilerError> PrepareSingleModule(MidoriTest::SourceFixture&& source)
	{
		MidoriResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		MidoriResult::ModuleManagerResult build_graph_result = ModuleManager(std::move(lex_result.value()), source.FileName(), source.SourceLines()).GenerateBuildGraph();
		if (!build_graph_result.has_value())
		{
			return std::unexpected(std::move(build_graph_result.error()));
		}

		BuildGraph build_graph = std::move(build_graph_result.value());
		std::unordered_map<std::string, BuildGraph::BuildNode>::iterator node_it = build_graph.m_nodes.find(source.FileName());
		if (node_it == build_graph.m_nodes.end())
		{
			return std::unexpected(CompilerError::Simple(CompilerStage::Module, "Failed to locate the prepared main module."));
		}

		std::optional<ModuleDeclaration> module_declaration = std::nullopt;
		std::unordered_map<std::string, ModuleDeclaration>::const_iterator module_it = build_graph.m_module_declarations.find(source.FileName());
		if (module_it != build_graph.m_module_declarations.end())
		{
			module_declaration = module_it->second;
		}

		TokenStream tokens = std::move(node_it->second.m_tokens);
		std::vector<UseImport> use_imports = std::move(node_it->second.m_use_imports);
		return PreparedModule(std::move(source), std::move(tokens), std::move(module_declaration), std::move(use_imports));
	}

	std::expected<PreparedTypedModule, MidoriResult::CompilerDiagnostics> TypeCheckPreparedModuleWithDiagnostics(PreparedModule&& prepared)
	{
		const ModuleDeclaration* module_declaration = prepared.m_module_declaration.has_value()
			? &prepared.m_module_declaration.value()
			: nullptr;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), {}, {}, prepared.m_use_imports, module_declaration);
		MidoriResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.has_value())
		{
			return std::unexpected(std::move(parse_result.error()));
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		MidoriResult::TypeCheckerResult typecheck_result = TypeChecker(std::move(parse_result).value(), prepared.m_source.FileName(), prepared.m_source.SourceLines()).TypeCheck();
		if (!typecheck_result.has_value())
		{
			return std::unexpected(std::move(typecheck_result.error()));
		}

		return PreparedTypedModule(std::move(prepared.m_source), std::move(typecheck_result).value(), std::move(warnings), std::move(prepared.m_module_declaration));
	}

	std::unordered_set<std::string> CollectExports(const std::optional<ModuleDeclaration>& module_declaration)
	{
		std::unordered_set<std::string> export_set;
		if (!module_declaration.has_value())
		{
			return export_set;
		}

		for (const ModuleExport& export_entry : module_declaration->Exports())
		{
			export_set.insert(export_entry.m_symbol_name);
		}

		return export_set;
	}
}

namespace MidoriTest
{
	LexedSnippet::LexedSnippet(SourceFixture source, TokenStream&& tokens)
		: m_source(std::move(source)),
		m_tokens(std::move(tokens))
	{
	}

	ParsedSnippet::ParsedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings)),
		m_module_declaration(std::move(module_declaration)),
		m_use_imports(std::move(use_imports))
	{
	}

	TypedSnippet::TypedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings))
	{
	}

	AnalyzedSnippet::AnalyzedSnippet(SourceFixture source, MidoriProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::vector<CompilerError>&& errors)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings)),
		m_errors(std::move(errors))
	{
	}

	ExecutedSnippet::ExecutedSnippet(SourceFixture source, int exit_code, CapturedOutput output)
		: m_source(std::move(source)),
		m_exit_code(exit_code),
		m_output(std::move(output))
	{
	}

	std::expected<LexedSnippet, CompilerError> LexSnippet(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));
		MidoriResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		return LexedSnippet(std::move(source), std::move(lex_result.value()));
	}

	std::expected<ParsedSnippet, CompilerError> ParseSnippet(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(std::move(prepared_result.error()));
		}

		PreparedModule prepared = std::move(prepared_result.value());
		const ModuleDeclaration* module_declaration = prepared.m_module_declaration.has_value()
			? &prepared.m_module_declaration.value()
			: nullptr;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), {}, {}, prepared.m_use_imports, module_declaration);
		MidoriResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.has_value())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return std::unexpected(std::move(parse_result.error()).TakeFirst());
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		return ParsedSnippet(std::move(prepared.m_source), std::move(parse_result.value()), std::move(warnings), std::move(prepared.m_module_declaration), std::move(prepared.m_use_imports));
	}

	std::expected<TypedSnippet, MidoriResult::CompilerDiagnostics> TypeCheckSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(prepared_result.error())));
		}

		std::expected<PreparedTypedModule, MidoriResult::CompilerDiagnostics> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.value()));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		PreparedTypedModule typed = std::move(typed_result.value());
		return TypedSnippet(std::move(typed.m_source), std::move(typed.m_program), std::move(typed.m_warnings));
	}

	std::expected<TypedSnippet, CompilerError> TypeCheckSnippet(std::string source_code, std::string file_name)
	{
		std::expected<TypedSnippet, MidoriResult::CompilerDiagnostics> typed_result = TypeCheckSnippetWithDiagnostics(std::move(source_code), std::move(file_name));
		if (!typed_result.has_value())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return std::unexpected(std::move(typed_result.error()).TakeFirst());
		}

		return std::move(typed_result.value());
	}

	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> GenerateBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(prepared_result.error())));
		}

		std::expected<PreparedTypedModule, MidoriResult::CompilerDiagnostics> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.value()));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		PreparedTypedModule typed = std::move(typed_result.value());
		std::string module_name = typed.m_module_declaration.has_value()
			? typed.m_module_declaration->ModuleName()
			: std::filesystem::path(typed.m_source.FileName()).stem().string();

		MidoriResult::CodeGeneratorResult codegen_result = CodeGenerator(
			std::move(typed.m_program),
			typed.m_source.FileName(),
			typed.m_source.SourceLines(),
			std::move(module_name),
			CollectExports(typed.m_module_declaration)).GenerateModuleBytecode();
		if (!codegen_result.has_value())
		{
			return std::unexpected(std::move(codegen_result.error()));
		}

		return std::move(codegen_result).value();
	}

	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> GenerateOptimizedBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(prepared_result.error())));
		}

		std::expected<PreparedTypedModule, MidoriResult::CompilerDiagnostics> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.value()));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		PreparedTypedModule typed = std::move(typed_result.value());

		// The real pipeline also runs StaticAnalyzerManager here (Compiler.cpp's
		// WithStaticAnalysis, between WithTypeCheckedAst and WithOptimizedAst).
		// Its four passes (ShadowingPolicy, UnusedLocal, CaptureEscape,
		// IntegerOverflow) only read the tree and write into a
		// DiagnosticSink for warnings/errors; none of them mutates the AST, so
		// no optimizer pass can depend on their output. It is intentionally
		// skipped here.
		MidoriResult::OptimizerResult optimize_result = OptimizerManager(std::move(typed.m_program)).Optimize();
		if (!optimize_result.has_value())
		{
			return std::unexpected(MidoriResult::CompilerDiagnostics(std::move(optimize_result.error())));
		}

		typed.m_program = std::move(optimize_result).value();

		std::string module_name = typed.m_module_declaration.has_value()
			? typed.m_module_declaration->ModuleName()
			: std::filesystem::path(typed.m_source.FileName()).stem().string();

		MidoriResult::CodeGeneratorResult codegen_result = CodeGenerator(
			std::move(typed.m_program),
			typed.m_source.FileName(),
			typed.m_source.SourceLines(),
			std::move(module_name),
			CollectExports(typed.m_module_declaration)).GenerateModuleBytecode();
		if (!codegen_result.has_value())
		{
			return std::unexpected(std::move(codegen_result.error()));
		}

		return std::move(codegen_result).value();
	}

	std::expected<AnalyzedSnippet, CompilerError> AnalyzeSnippet(std::string source_code, std::string file_name)
	{
		std::expected<TypedSnippet, CompilerError> typed_result = TypeCheckSnippet(std::move(source_code), std::move(file_name));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		TypedSnippet typed = std::move(typed_result.value());
		StaticAnalysisResult analysis_result = StaticAnalyzerManager().Analyze(typed.m_program, typed.m_source.FileName(), typed.m_source.SourceLines());
		return AnalyzedSnippet(std::move(typed.m_source), std::move(typed.m_program), std::move(analysis_result.m_warnings), std::move(analysis_result.m_errors));
	}

	MidoriResult::CompilationResult CompileSnippetWithReport(std::string source_code, std::string file_name)
	{
		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		return MidoriDriver::CompileSourceWithReport(std::move(source_code), std::move(file_name));
	}

	const MidoriResult::CompilerReport& CompilationReport(const MidoriResult::CompilationResult& compilation_result)
	{
		if (compilation_result.has_value())
		{
			return compilation_result->Report();
		}

		return compilation_result.error();
	}

	MidoriResult::CompilerResult CompileSnippet(std::string source_code, std::string file_name)
	{
		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		return MidoriDriver::CompileSource(std::move(source_code), std::move(file_name));
	}

	std::expected<ExecutedSnippet, CompilerError> ExecuteSnippet(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));
		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		MidoriResult::CompilerResult compile_result = MidoriDriver::CompileSource(std::string(source.SourceCode()), source.FileName());
		if (!compile_result.has_value())
		{
			// Execution helpers keep a single-error surface for legacy tests; compile-time
			// collections are narrowed explicitly at this boundary.
			return std::unexpected(std::move(compile_result.error()).TakeFirst());
		}

		OutputCapture capture;
		MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(compile_result.value()));
		if (!run_result.has_value())
		{
			const RuntimeError runtime_error = run_result.error();
			std::print("{}", runtime_error.Rendered());
			CapturedOutput output = capture.Stop();
			return ExecutedSnippet(std::move(source), runtime_error.ExitCode(), std::move(output));
		}
		CapturedOutput output = capture.Stop();

		return ExecutedSnippet(std::move(source), run_result.value(), std::move(output));
	}

	std::vector<Token::Name> CollectTokenNames(const TokenStream& tokens, bool include_whitespace, bool include_end_of_file)
	{
		std::vector<Token::Name> token_names;
		for (int index = 0; index < tokens.Size(); index += 1)
		{
			const Token& token = tokens[index];
			if (!include_whitespace && token.m_token_name == Token::Name::WHITESPACE)
			{
				continue;
			}

			if (!include_end_of_file && token.m_token_name == Token::Name::END_OF_FILE)
			{
				continue;
			}

			token_names.emplace_back(token.m_token_name);
		}

		return token_names;
	}
}
