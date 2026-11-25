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
#include "Support/Expected.h"
#include "Support/Optional.h"

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
		MarmotCompiler::Optional<ModuleDeclaration> m_module_declaration;
		std::vector<UseImport> m_use_imports;

		PreparedModule(MarmotTest::SourceFixture source, TokenStream&& tokens, MarmotCompiler::Optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
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
		MarmotCompiler::Optional<ModuleDeclaration> m_module_declaration;

		PreparedTypedModule(MarmotTest::SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings, MarmotCompiler::Optional<ModuleDeclaration>&& module_declaration)
			: m_source(std::move(source)),
			m_program(std::move(program)),
			m_warnings(std::move(warnings)),
			m_module_declaration(std::move(module_declaration))
		{
		}
	};

	MarmotResult::Result<PreparedModule> PrepareSingleModule(MarmotTest::SourceFixture&& source)
	{
		MarmotResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(lex_result.Error()));
		}

		MarmotResult::ModuleManagerResult build_graph_result = ModuleManager(std::move(lex_result.Value()), source.FileName(), source.SourceLines(), MarmotDriver::EnvironmentCompilationInputs()).GenerateBuildGraph();
		if (!build_graph_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(build_graph_result.Error()));
		}

		BuildGraph build_graph = std::move(build_graph_result.Value());
		std::unordered_map<std::string, BuildGraph::BuildNode>::iterator node_it = build_graph.m_nodes.find(source.FileName());
		if (node_it == build_graph.m_nodes.end())
		{
			return MarmotCompiler::Failure(CompilerError::Simple(CompilerStage::Module, "Failed to locate the prepared main module."));
		}

		MarmotCompiler::Optional<ModuleDeclaration> module_declaration = MarmotCompiler::None;
		std::unordered_map<std::string, ModuleDeclaration>::const_iterator module_it = build_graph.m_module_declarations.find(source.FileName());
		if (module_it != build_graph.m_module_declarations.end())
		{
			module_declaration = module_it->second;
		}

		TokenStream tokens = std::move(node_it->second.m_tokens);
		std::vector<UseImport> use_imports = std::move(node_it->second.m_use_imports);
		return PreparedModule(std::move(source), std::move(tokens), std::move(module_declaration), std::move(use_imports));
	}

	MarmotResult::DiagnosticsResult<PreparedTypedModule> TypeCheckPreparedModuleWithDiagnostics(PreparedModule&& prepared)
	{
		const ModuleDeclaration* module_declaration = prepared.m_module_declaration.HasValue()
			? &prepared.m_module_declaration.Value()
			: nullptr;
		const ModuleImports imports;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), imports, prepared.m_use_imports, module_declaration);
		MarmotResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(parse_result.Error()));
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		MarmotResult::TypeCheckerResult typecheck_result = TypeChecker(std::move(parse_result).Value(), prepared.m_source.FileName(), prepared.m_source.SourceLines()).TypeCheck();
		if (!typecheck_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(typecheck_result.Error()));
		}

		return PreparedTypedModule(std::move(prepared.m_source), std::move(typecheck_result).Value(), std::move(warnings), std::move(prepared.m_module_declaration));
	}

	std::unordered_set<std::string> CollectExports(const MarmotCompiler::Optional<ModuleDeclaration>& module_declaration)
	{
		std::unordered_set<std::string> export_set;
		if (!module_declaration.HasValue())
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

	ParsedSnippet::ParsedSnippet(SourceFixture source, MarmotProgramTree&& program, std::vector<CompilerWarning>&& warnings, MarmotCompiler::Optional<ModuleDeclaration>&& module_declaration, std::vector<UseImport>&& use_imports)
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

	MarmotResult::Result<LexedSnippet> LexSnippet(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));
		MarmotResult::LexerResult lex_result = Lexer(std::string(source.SourceCode()), source.FileName()).Lex();
		if (!lex_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(lex_result.Error()));
		}

		return LexedSnippet(std::move(source), std::move(lex_result.Value()));
	}

	MarmotResult::Result<ParsedSnippet> ParseSnippet(std::string source_code, std::string file_name)
	{
		MarmotResult::Result<PreparedModule> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(prepared_result.Error()));
		}

		PreparedModule prepared = std::move(prepared_result.Value());
		const ModuleDeclaration* module_declaration = prepared.m_module_declaration.HasValue()
			? &prepared.m_module_declaration.Value()
			: nullptr;
		const ModuleImports imports;
		Parser parser(std::move(prepared.m_tokens), prepared.m_source.FileName(), prepared.m_source.SourceLines(), imports, prepared.m_use_imports, module_declaration);
		MarmotResult::ParserResult parse_result = parser.Parse();
		if (!parse_result.HasValue())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return MarmotCompiler::Failure(std::move(parse_result.Error()).TakeFirst());
		}

		std::vector<CompilerWarning> warnings = parser.GetWarnings();
		return ParsedSnippet(std::move(prepared.m_source), std::move(parse_result.Value()), std::move(warnings), std::move(prepared.m_module_declaration), std::move(prepared.m_use_imports));
	}

	MarmotResult::DiagnosticsResult<TypedSnippet> TypeCheckSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		MarmotResult::Result<PreparedModule> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.HasValue())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(prepared_result.Error())));
		}

		MarmotResult::DiagnosticsResult<PreparedTypedModule> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.Value()));
		if (!typed_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(typed_result.Error()));
		}

		PreparedTypedModule typed = std::move(typed_result.Value());
		return TypedSnippet(std::move(typed.m_source), std::move(typed.m_program), std::move(typed.m_warnings));
	}

	MarmotResult::Result<TypedSnippet> TypeCheckSnippet(std::string source_code, std::string file_name)
	{
		MarmotResult::DiagnosticsResult<TypedSnippet> typed_result = TypeCheckSnippetWithDiagnostics(std::move(source_code), std::move(file_name));
		if (!typed_result.HasValue())
		{
			// Test helpers still offer single-error adapters for legacy callers; preserve
			// structured diagnostics upstream and narrow only at this boundary.
			return MarmotCompiler::Failure(std::move(typed_result.Error()).TakeFirst());
		}

		return std::move(typed_result.Value());
	}

	MarmotResult::DiagnosticsResult<LoweredModule> LowerSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		MarmotResult::Result<PreparedModule> prepared_result = PrepareSingleModule(SourceFixture(std::move(source_code), std::move(file_name)));
		if (!prepared_result.HasValue())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(std::move(prepared_result.Error())));
		}

		MarmotResult::DiagnosticsResult<PreparedTypedModule> typed_result =
			TypeCheckPreparedModuleWithDiagnostics(std::move(prepared_result.Value()));
		if (!typed_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(typed_result.Error()));
		}

		PreparedTypedModule typed = std::move(typed_result.Value());
		const std::string module_name = typed.m_module_declaration.HasValue()
			? typed.m_module_declaration->ModuleName()
			: std::filesystem::path(typed.m_source.FileName()).stem().string();
		const std::unordered_set<std::string> exports = CollectExports(typed.m_module_declaration);
		const LoweringImports imports;
		return Lowering(typed.m_program, typed.m_source.FileName(), typed.m_source.SourceLines(), module_name, exports, imports).Lower();
	}

	MarmotResult::DiagnosticsResult<BytecodeModule> GenerateBytecodeSnippetWithDiagnostics(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));
		MarmotResult::DiagnosticsResult<LoweredModule> lowered = LowerSnippetWithDiagnostics(std::string(source.SourceCode()), source.FileName());
		if (!lowered.HasValue())
		{
			return MarmotCompiler::Failure(std::move(lowered.Error()));
		}

		MarmotCompiler::Expected<void, MarmotIROptimizationFailure> optimized = MarmotIROptimizer().Optimize(lowered->m_module);
		if (!optimized.HasValue())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerDiagnostics(CompilerError::Simple(CompilerStage::Lowering, std::format("{} produced invalid MarmotIR.", optimized.Error().m_producer), CompilerErrorCode::CompilerInternalError)));
		}

		return BytecodeBackend(lowered.Value(), source.FileName(), source.SourceLines()).Emit();
	}

	MarmotResult::Result<AnalyzedSnippet> AnalyzeSnippet(std::string source_code, std::string file_name)
	{
		MarmotResult::Result<TypedSnippet> typed_result = TypeCheckSnippet(std::move(source_code), std::move(file_name));
		if (!typed_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(typed_result.Error()));
		}

		TypedSnippet typed = std::move(typed_result.Value());
		std::vector<CompilerWarning> warnings = StaticAnalyzerManager().Analyze(typed.m_program, typed.m_source.FileName(), typed.m_source.SourceLines());
		return AnalyzedSnippet(std::move(typed.m_source), std::move(typed.m_program), std::move(warnings));
	}

	MarmotResult::CompiledProgramReportResult CompileSnippetWithReport(std::string source_code, std::string file_name)
	{

		return MarmotDriver::CompileSourceWithReport(std::move(source_code), std::move(file_name));
	}

	const MarmotResult::CompilerReport& CompilationReport(const MarmotResult::CompiledProgramReportResult& compilation_result)
	{
		if (compilation_result.HasValue())
		{
			return compilation_result->Report();
		}

		return compilation_result.Error();
	}

	MarmotResult::ExecutableDiagnosticsResult CompileSnippet(std::string source_code, std::string file_name)
	{
		return MarmotDriver::CompileSourceWithReport(std::move(source_code), std::move(file_name))
			.Transform([](MarmotResult::CompiledProgram&& program) { return std::move(program).TakeExecutable(); })
			.TransformError([](MarmotResult::CompilerReport&& report) { return std::move(report.m_errors); });
	}

	MarmotResult::Result<ExecutedSnippet> ExecuteSnippet(std::string source_code, std::string file_name)
	{
		SourceFixture source(std::move(source_code), std::move(file_name));

		MarmotResult::CompiledProgramReportResult compile_result = MarmotDriver::CompileSourceWithReport(
			std::string(source.SourceCode()), source.FileName(),
			MarmotDriver::EnvironmentCompilationInputs().WithNativeLibraryPolicies({ { "marmotstdlib", NativeLibraryPolicy{ .m_thread_safe = true } } }));
		if (!compile_result.HasValue())
		{
			// Execution helpers keep a single-error surface for legacy tests; compile-time
			// collections are narrowed explicitly at this boundary.
			return MarmotCompiler::Failure(std::move(compile_result.Error().m_errors).TakeFirst());
		}

		std::pair<int, CapturedOutput> run = RunExecutable(std::move(compile_result.Value()).TakeExecutable());
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
