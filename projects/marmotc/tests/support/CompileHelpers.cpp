#include "support/CompileHelpers.h"

#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/MarmotIROptimizer/MarmotIROptimizer.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"
#include "Utility/Driver/MarmotDriver.h"
#include "Bytecode/Artifact/BinaryArtifact.h"

#include <filesystem>
#include <format>
#include <print>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
	struct PreparedModule
	{
		MarmotTest::SourceFixture m_source;
		TokenStream m_tokens;
		std::optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;

		PreparedModule(MarmotTest::SourceFixture source, TokenStream&& tokens, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
			: m_source(std::move(source)),
			m_tokens(std::move(tokens)),
			m_module_declaration(std::move(module_declaration)),
			m_use_imports(std::move(use_imports))
		{
		}
	};

	struct PreparedTypedModule
	{
		MarmotTest::SourceFixture m_source;
		MarmotProgramTree m_program;
		std::vector<CompilerWarning> m_warnings;
		std::optional<ModuleDeclaration> m_module_declaration;

		PreparedTypedModule(MarmotTest::SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration)
			: m_source(std::move(source)),
			m_program(std::move(program)),
			m_warnings(std::move(warnings)),
			m_module_declaration(std::move(module_declaration))
		{
		}
	};

	std::expected<PreparedModule, CompilerError> PrepareSingleModule(MarmotTest::SourceFixture&& source)
	{
		MarmotResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		MarmotResult::ModuleManagerResult build_graph_result = ModuleManager(std::move(lex_result.value()), source.FileName(), source.SourceLines(), MarmotDriver::EnvironmentCompilationInputs()).GenerateBuildGraph();
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

	std::expected<PreparedTypedModule, MarmotResult::CompilerDiagnostics> TypeCheckPreparedModuleWithDiagnostics(PreparedModule&& prepared)
	{
		const ModuleDeclaration* module_declaration = prepared.m_module_declaration.has_value()
			? &prepared.m_module_declaration.value()
			: nullptr;
		const ModuleImports imports;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), imports, prepared.m_use_imports, module_declaration);
		MarmotResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.has_value())
		{
			return std::unexpected(std::move(parse_result.error()));
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		MarmotResult::TypeCheckerResult typecheck_result = TypeChecker(std::move(parse_result).value(), prepared.m_source.FileName(), prepared.m_source.SourceLines()).TypeCheck();
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

namespace MarmotTest
{
	LexedSnippet::LexedSnippet(SourceFixture source, TokenStream&& tokens)
		: m_source(std::move(source)),
		m_tokens(std::move(tokens))
	{
	}

	ParsedSnippet::ParsedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings, std::optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings)),
		m_module_declaration(std::move(module_declaration)),
		m_use_imports(std::move(use_imports))
	{
	}

	TypedSnippet::TypedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings))
	{
	}

	AnalyzedSnippet::AnalyzedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings)
		: m_source(std::move(source)),
		m_program(std::move(program)),
		m_warnings(std::move(warnings))
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
		MarmotResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
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
		const ModuleImports imports;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), imports, prepared.m_use_imports, module_declaration);
		MarmotResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.has_value())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return std::unexpected(std::move(parse_result.error()).TakeFirst());
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		return ParsedSnippet(std::move(prepared.m_source), std::move(parse_result.value()), std::move(warnings), std::move(prepared.m_module_declaration), std::move(prepared.m_use_imports));
	}

	std::expected<TypedSnippet, MarmotResult::CompilerDiagnostics> TypeCheckSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(prepared_result.error())));
		}

		std::expected<PreparedTypedModule, MarmotResult::CompilerDiagnostics> typed_result =
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
		std::expected<TypedSnippet, MarmotResult::CompilerDiagnostics> typed_result = TypeCheckSnippetWithDiagnostics(std::move(source_code), std::move(file_name));
		if (!typed_result.has_value())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return std::unexpected(std::move(typed_result.error()).TakeFirst());
		}

		return std::move(typed_result.value());
	}

	std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> LowerSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		std::expected<PreparedModule, CompilerError> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.has_value())
		{
			return std::unexpected(MarmotResult::CompilerDiagnostics(std::move(prepared_result.error())));
		}

		std::expected<PreparedTypedModule, MarmotResult::CompilerDiagnostics> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.value()));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		PreparedTypedModule typed = std::move(typed_result.value());
		const std::string module_name = typed.m_module_declaration.has_value()
			? typed.m_module_declaration->ModuleName()
			: std::filesystem::path(typed.m_source.FileName()).stem().string();
		const std::unordered_set<std::string> exports = CollectExports(typed.m_module_declaration);
		const LoweringImports imports;
		return Lowering(typed.m_program, typed.m_source.FileName(), typed.m_source.SourceLines(), module_name, exports, imports).Lower();
	}

	std::expected<BytecodeModule, MarmotResult::CompilerDiagnostics> GenerateBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));
		std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> lowered = LowerSnippetWithDiagnostics(std::string(source.SourceCode()), source.FileName());
		if (!lowered.has_value())
		{
			return std::unexpected(std::move(lowered.error()));
		}

		std::expected<void, MarmotIRPassFailure> optimized = MarmotIROptimizer().Optimize(lowered->m_module);
		if (!optimized.has_value())
		{
			return std::unexpected(MarmotResult::CompilerDiagnostics(CompilerError::Simple(CompilerStage::Lowering, std::format("The MarmotIR pass {} produced invalid MarmotIR.", optimized.error().m_pass), CompilerErrorCode::CompilerInternalError)));
		}

		return BytecodeBackend(lowered.value(), source.FileName(), source.SourceLines()).Emit();
	}

	std::expected<AnalyzedSnippet, CompilerError> AnalyzeSnippet(std::string source_code, std::string file_name)
	{
		std::expected<TypedSnippet, CompilerError> typed_result = TypeCheckSnippet(std::move(source_code), std::move(file_name));
		if (!typed_result.has_value())
		{
			return std::unexpected(std::move(typed_result.error()));
		}

		TypedSnippet typed = std::move(typed_result.value());
		const WorkerCrossings crossings = FindWorkerCrossings(typed.m_program, {});
		std::vector<CompilerWarning> warnings = StaticAnalyzerManager(crossings).Analyze(typed.m_program, typed.m_source.FileName(), typed.m_source.SourceLines());
		return AnalyzedSnippet(std::move(typed.m_source), std::move(typed.m_program), std::move(warnings));
	}

	MarmotResult::CompilationResult CompileSnippetWithReport(std::string source_code, std::string file_name)
	{

		return MarmotDriver::CompileSourceWithReport(std::move(source_code), std::move(file_name));
	}

	const MarmotResult::CompilerReport& CompilationReport(const MarmotResult::CompilationResult& compilation_result)
	{
		if (compilation_result.has_value())
		{
			return compilation_result->Report();
		}

		return compilation_result.error();
	}

	MarmotResult::CompilerResult CompileSnippet(std::string source_code, std::string file_name)
	{
		return MarmotDriver::CompileSourceWithReport(std::move(source_code), std::move(file_name))
			.transform([](MarmotResult::CompiledProgram&& program) { return std::move(program).TakeExecutable(); })
			.transform_error([](MarmotResult::CompilerReport&& report) { return std::move(report.m_errors); });
	}

	std::expected<ExecutedSnippet, CompilerError> ExecuteSnippet(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));

		MarmotResult::CompilationResult compile_result = MarmotDriver::CompileSourceWithReport(std::string(source.SourceCode()), source.FileName(), MarmotDriver::EnvironmentCompilationInputs());
		if (!compile_result.has_value())
		{
			// Execution helpers keep a single-error surface for legacy tests; compile-time
			// collections are narrowed explicitly at this boundary.
			return std::unexpected(std::move(compile_result.error().m_errors).TakeFirst());
		}

		std::pair<int, CapturedOutput> run = RunExecutable(std::move(compile_result.value()).TakeExecutable());
		return ExecutedSnippet(std::move(source), run.first, std::move(run.second));
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
