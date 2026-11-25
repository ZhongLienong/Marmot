#include "Utility/Diagnostics/Diagnostics.h"
#include "Support/Terminal/Terminal.h"
#include "Compiler/Source/Source.h"
#include "Compiler.h"
#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/CompilationTimings/CompilationTimings.h"
#include "Compiler/CompilationSchedule/CompilationSchedule.h"
#include "Compiler/CompilationSchedule/CompilationExecution.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/AbstractSyntaxTree/Printer/AbstractSyntaxTreePrinter.h"
#include "Compiler/MarmotIR/Printer/MarmotIRPrinter.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "Compiler/MarmotIROptimizer/MarmotIROptimizer.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Module/ModuleExports.h"
#include "Compiler/Module/ModuleInterfaceRegistry.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <print>
#include <ranges>
#include <sstream>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>

namespace
{
	enum class CompilationMode { Build, Analysis };

	struct ParsedModule
	{
		MarmotProgramTree m_ast;
		ModuleExports m_exports;
		std::vector<CompilerWarning> m_warnings;
	};

	struct ModuleCompilationInput
	{
		BuildGraph::BuildNode m_source;
		MarmotCompiler::Optional<ModuleDeclaration> m_declaration;
		std::vector<std::shared_ptr<const ModuleInterface>> m_dependencies;

		ModuleCompilationInput(BuildGraph::BuildNode source, MarmotCompiler::Optional<ModuleDeclaration> declaration, std::vector<std::shared_ptr<const ModuleInterface>> dependencies)
			: m_source(std::move(source)),
			m_declaration(std::move(declaration)),
			m_dependencies(std::move(dependencies))
		{
		}
	};

	struct ModuleCompilationOptions
	{
		bool m_emit_bytecode;
		bool m_emit_marmot_ir;
		bool m_emit_ast;
		CompilationTimings& m_timings;

		ModuleCompilationOptions(const CompilationInputs& inputs, CompilationTimings& timings, CompilationMode mode)
			: m_emit_bytecode(mode == CompilationMode::Build),
			m_emit_marmot_ir(inputs.EmitsMarmotIR()),
			m_emit_ast(inputs.EmitsAst()),
			m_timings(timings)
		{
		}
	};

	struct TypeCheckedModule
	{
		MarmotProgramTree m_ast;
		ModuleExports m_exports;
		std::unordered_map<int, std::string> m_generic_parameter_names;
	};

	struct BuildGraphArtifacts
	{
		std::vector<BytecodeModule> m_bytecode_modules;
		std::vector<std::string> m_marmot_ir;
		std::vector<std::string> m_ast;
		MarmotResult::CompilerWarnings m_warnings;
		EditorSymbols m_symbols;
	};

	static MarmotResult::Result<ModuleImports> BuildImportContext(const ModuleCompilationInput& input)
	{
		return ModuleImports::Collect(input.m_dependencies).TransformError
		(
			[&input](ModuleImportConflict conflict)
			{
				const BuildGraph::BuildNode& source = input.m_source;
				return CompilerError::WithToken(CompilerStage::Module, conflict.m_message, source.m_import_tokens.at(conflict.m_dependency.string()), source.m_file_name, source.m_source_lines);
			}
		);
	}

	static MarmotResult::DiagnosticsResult<ParsedModule> ParseModule(TokenStream&& tokens, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ModuleImports& import_context, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl)
	{
		Parser parser(std::move(tokens), file_path, module_source_lines, import_context, use_imports, module_decl);
		MarmotResult::ParserResult ast = parser.Parse();
		if (!ast.HasValue())
		{
			return MarmotCompiler::Failure(std::move(ast.Error()));
		}

		std::unordered_set<std::string> export_set_for_types;
		if (module_decl)
		{
			for (const ModuleExport& exp : module_decl->Exports())
			{
				export_set_for_types.insert(exp.m_symbol_name);
			}
		}

		TypeChecker::TypeEnvironment type_signatures = TypeChecker::ExtractTypeSignatures(ast.Value(), module_decl ? &export_set_for_types : nullptr);

		std::vector<CompilerWarning> warnings = parser.GetWarnings();

		return ParsedModule
		{
			std::move(*ast),
			ModuleExports(module_decl, file_path, std::move(type_signatures), parser.GetTypeclassMetadata()),
			std::move(warnings)
		};
	}

	static MarmotResult::DiagnosticsResult<TypeCheckedModule> TypeCheckModule(ParsedModule parsed, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ModuleImports& import_context)
	{
		TypeChecker type_checker(std::move(parsed.m_ast), file_path, module_source_lines, import_context.Types(), import_context.Typeclasses());
		MarmotResult::TypeCheckerResult typecheck_result = type_checker.TypeCheck();
		if (!typecheck_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(typecheck_result.Error()));
		}

		parsed.m_exports.RecordInferredTypes(type_checker.ModuleTypes());
		return TypeCheckedModule{ std::move(typecheck_result.Value()), std::move(parsed.m_exports), std::move(type_checker).TakeGenericParameterNames() };
	}

	// A violation is the compiler's bug, not the program's, so every one is
	// reported and the module stops.
	static CompilerError InvalidMarmotIR(const MarmotIRModule& module, const std::vector<MarmotIRViolation>& violations, std::string_view what, const std::string& file_path)
	{

		const std::string message = violations
			| std::views::transform([](const MarmotIRViolation& violation) { return violation.ToString(); })
			| std::views::join_with('\n')
			| std::ranges::to<std::string>();
		const MarmotIRPrinter printer(module);
		const std::string functions = module.m_functions
			| std::views::filter([&violations](const MarmotIRFunction& function)
			{
				return std::ranges::any_of(violations, [&function](const MarmotIRViolation& violation) { return violation.m_function == function.m_name; });
			})
			| std::views::transform([&printer](const MarmotIRFunction& function) { return printer.PrintFunction(function); })
			| std::views::join_with('\n')
			| std::ranges::to<std::string>();
		return CompilerError::WithFile(CompilerStage::Lowering, std::format("{} produced invalid MarmotIR:\n{}\n\n{}", what, message, functions), file_path, CompilerErrorCode::CompilerInternalError);
	}

	using InterfaceReady = CompilationExecution::InterfaceReady;
	using InterfacePublisher = std::function<void(std::shared_ptr<const ModuleInterface>)>;

	static MarmotResult::CompilerReport InternalError(const std::string& file_path, std::string_view where, std::string_view detail)
	{
		return MarmotResult::CompilerReport(CompilerError::WithFile(CompilerStage::Compiler, std::format("Internal compiler error {}: {}", where, detail), file_path, CompilerErrorCode::CompilerInternalError));
	}

	class ModuleCompiler
	{
	public:
		ModuleCompiler(const CompilationInputs& inputs, CompilationTimings& timings, CompilationMode mode)
			: m_options(inputs, timings, mode)
		{
		}

		MarmotResult::CompiledModuleReportResult Compile(ModuleCompilationInput input, const InterfacePublisher& publish) const
		{
			const std::string& file_path = input.m_source.m_file_name;
			MarmotResult::CompilerWarnings warnings;
			// Only reporting and timing span phases; each phase owns its result.
			const auto run_stage = [&]<typename Fn>(std::string_view name, CompilationTimings::Phase phase, Fn&& run)
				-> MarmotResult::ReportResult<typename std::invoke_result_t<Fn>::ValueType>
			{
				CompilationTimings::Timer timer(m_options.m_timings, phase);
				try
				{
					return run().TransformError([&warnings](auto diagnostics)
					{
						return MarmotResult::CompilerReport(std::move(warnings), MarmotResult::CompilerDiagnostics(std::move(diagnostics)));
					});
				}
				// Worker exceptions must become diagnostics while their phase is known.
				catch (const std::exception& e)
				{
					return MarmotCompiler::Failure(InternalError(file_path, std::format("in the {} stage", name), e.what()));
				}
				catch (...)
				{
					return MarmotCompiler::Failure(InternalError(file_path, std::format("in the {} stage", name), "unknown exception"));
				}
			};

			MarmotResult::ReportResult<ModuleImports> imports = run_stage("import context", CompilationTimings::Phase::Imports, [&]()
			{
				return BuildImportContext(input);
			});
			if (!imports.HasValue())
			{
				return MarmotCompiler::Failure(std::move(imports.Error()));
			}
			const std::vector<std::string> source_lines = [&]()
			{
				CompilationTimings::Timer timer(m_options.m_timings, CompilationTimings::Phase::SourceLines);
				return std::move(input.m_source.m_source_lines);
			}();
			const std::vector<int> doc_comment_lines = std::move(input.m_source.m_tokens).TakeDocCommentLines();

			MarmotResult::ReportResult<ParsedModule> parsed = run_stage
			(
				"parser",
				CompilationTimings::Phase::Parsing,
				[&]()
				{
					const ModuleDeclaration* declaration = input.m_declaration.HasValue() ? &*input.m_declaration : nullptr;
					return ParseModule(std::move(input.m_source.m_tokens), file_path, source_lines, *imports, input.m_source.m_use_imports, declaration)
						.Transform
						(
							[&warnings](ParsedModule module)
							{
								warnings.Append(std::move(module.m_warnings));
								return module;
							}
						);
				}
			);
			if (!parsed.HasValue())
			{
				return MarmotCompiler::Failure(std::move(parsed.Error()));
			}
			MarmotResult::ReportResult<TypeCheckedModule> checked = run_stage("type checker", CompilationTimings::Phase::TypeChecking, [&]()
			{
				return TypeCheckModule(std::move(parsed).Value(), file_path, source_lines, *imports);
			});
			if (!checked.HasValue())
			{
				return MarmotCompiler::Failure(std::move(checked.Error()));
			}

			MarmotResult::ReportResult<void> analyzed = run_stage
			(
				"static analyzer",
				CompilationTimings::Phase::StaticAnalysis,
				[&]() -> MarmotResult::Result<void>
				{
					warnings.Append(StaticAnalyzerManager().Analyze(checked->m_ast, file_path, source_lines));
					return {};
				}
			);
			if (!analyzed.HasValue())
			{
				return MarmotCompiler::Failure(std::move(analyzed.Error()));
			}

			MarmotCompiler::Optional<ModuleDefinitions> definitions;
			if (!m_options.m_emit_bytecode)
			{
				definitions = ModuleDefinitions::Collect(checked->m_ast, checked->m_exports.ModuleName(), checked->m_generic_parameter_names, doc_comment_lines);
			}

			std::string ast_text;
			MarmotResult::ReportResult<LoweredModule> lowered = run_stage
			(
				"lowering",
				CompilationTimings::Phase::Lowering,
				[&]()
				{
					const ModuleExports& exports = checked->m_exports;
					const LoweringImports lowering_imports{ imports->Generics(), imports->Typeclasses() };
					if (m_options.m_emit_ast)
					{
						ast_text = AbstractSyntaxTreePrinter(exports.ModuleName(), checked->m_ast).Print();
					}
					return Lowering(checked->m_ast, file_path, source_lines, exports.ModuleName(), exports.Names(), lowering_imports).Lower();
				}
			);
			if (!lowered.HasValue())
			{
				return MarmotCompiler::Failure(std::move(lowered.Error()));
			}

			MarmotResult::ReportResult<PublishedModuleInterface> published = run_stage("module interface", CompilationTimings::Phase::Interface, [&]()
			{
				return std::move(checked->m_exports).Publish(*lowered, *imports, input.m_source.m_use_imports, source_lines);
			});
			if (!published.HasValue())
			{
				return MarmotCompiler::Failure(std::move(published.Error()));
			}
			// Importers need the interface, not this module's completed backend.
			publish(published->m_interface);
			if (!m_options.m_emit_bytecode)
			{
				definitions->m_reexports = std::move(published->m_reexports);
				return CompiledModule(std::move(published->m_interface))
					.WithDefinitions(std::move(*definitions))
					.WithWarnings(std::move(warnings).TakeAll());
			}

#if MARMOT_ENABLE_OPTIMIZER_STATS
			std::string optimizer_log;
#endif
			MarmotResult::ReportResult<std::string> marmot_ir = run_stage
			(
				"optimization",
				CompilationTimings::Phase::Optimization,
				[&]() -> MarmotResult::Result<std::string>
				{
					MarmotIROptimizer optimizer;
					MarmotCompiler::Expected<void, MarmotIROptimizationFailure> optimized = optimizer.Optimize(lowered->m_module);
					if (!optimized.HasValue())
					{
						return MarmotCompiler::Failure(InvalidMarmotIR(lowered->m_module, optimized.Error().m_violations, optimized.Error().m_producer, file_path));
					}
	#if MARMOT_ENABLE_OPTIMIZER_STATS
					optimizer_log = optimizer.Log();
	#endif
					return m_options.m_emit_marmot_ir ? MarmotIRPrinter(lowered->m_module).Print() : std::string();
				}
			);
			if (!marmot_ir.HasValue())
			{
				return MarmotCompiler::Failure(std::move(marmot_ir.Error()));
			}

			MarmotResult::ReportResult<BytecodeModule> bytecode = run_stage
			(
				"bytecode backend",
				CompilationTimings::Phase::Backend,
				[&]()
				{
					LoweredModule module = std::move(lowered).Value();
					return BytecodeBackend(module, file_path, source_lines).Emit().Transform
					(
						[&published](BytecodeModule emitted)
						{
							emitted.m_reexports = std::move(published->m_reexports);
							return emitted;
						}
					);
				}
			);
			if (!bytecode.HasValue())
			{
				return MarmotCompiler::Failure(std::move(bytecode.Error()));
			}

			CompilationTimings::Timer finalization(m_options.m_timings, CompilationTimings::Phase::Finalization);
			CompiledModule compiled = CompiledModule(std::move(published->m_interface))
				.WithWarnings(std::move(warnings).TakeAll())
				.WithBytecode(std::move(bytecode).Value())
				.WithMarmotIR(std::move(marmot_ir).Value())
				.WithAst(std::move(ast_text));
#if MARMOT_ENABLE_OPTIMIZER_STATS
			if (CompilerDiagnostics::StatisticsEnabled())
			{
				std::lock_guard<std::mutex> lock(m_print_mutex);
				std::print(stderr, "MarmotIR optimizer: {}\n{}", std::filesystem::path(file_path).filename().string(), optimizer_log);
			}
#endif
			return compiled;
		}

	private:
		ModuleCompilationOptions m_options;
#if MARMOT_ENABLE_OPTIMIZER_STATS
		mutable std::mutex m_print_mutex;
#endif
	};

	using ModuleFailures = std::unordered_map<std::string, MarmotResult::CompilerReport>;

	// Warnings and errors in build-schedule order (tier order, then file path
	// order). A module that failed keeps its warnings at its own position.
	static MarmotResult::CompilerReport CollectFailureReport(const CompilationSchedule& schedule, const std::unordered_map<std::string, CompiledModule>& compiled_modules, ModuleFailures&& failures)
	{
		MarmotResult::CompilerReport report;
		for (const std::string& file_path : schedule.m_tiers | std::views::join)
		{
			const ModuleFailures::iterator failed = failures.find(file_path);
			if (failed != failures.end())
			{
				report.Append(std::move(failed->second));
				continue;
			}

			const std::unordered_map<std::string, CompiledModule>::const_iterator compiled = compiled_modules.find(file_path);
			if (compiled != compiled_modules.end())
			{
				report.m_warnings.Append(compiled->second.Warnings());
			}
		}
		return report;
	}

	static MarmotResult::CompiledModuleReportResult CompileGraphModule(BuildGraph& build_graph, ModuleInterfaceRegistry& interfaces, const ModuleCompiler& compiler, const std::string& file_path, const InterfaceReady& interface_ready)
	{
		// Include input preparation in the same exception handling on the caller
		// and on workers, where an escaping exception would terminate the process.
		try
		{
			BuildGraph::BuildNode& node = build_graph.m_nodes.at(file_path);
			std::vector<std::shared_ptr<const ModuleInterface>> dependencies = interfaces.Snapshot(node.m_dependencies);
			MarmotCompiler::Optional<ModuleDeclaration> declaration;
			const std::unordered_map<std::string, ModuleDeclaration>::iterator declared = build_graph.m_module_declarations.find(file_path);
			if (declared != build_graph.m_module_declarations.end())
			{
				declaration.Emplace(std::move(declared->second));
			}
			ModuleCompilationInput input(std::move(node), std::move(declaration), std::move(dependencies));
			return compiler.Compile(std::move(input), [&interfaces, &file_path, &interface_ready](std::shared_ptr<const ModuleInterface> interface)
			{
				interfaces.Publish(file_path, std::move(interface));
				interface_ready(file_path);
			});
		}
		catch (const std::exception& e)
		{
			return MarmotCompiler::Failure(InternalError(file_path, "while compiling the module", e.what()));
		}
		catch (...)
		{
			return MarmotCompiler::Failure(InternalError(file_path, "while compiling the module", "unknown exception"));
		}
	}

	// Keep compiling eligible modules after failures so worker timing cannot
	// change which diagnostics are collected.
	static CompilationExecution::Results CompileModules(BuildGraph& build_graph, CompilationSchedule& schedule, const CompilationInputs& inputs, CompilationTimings& timings, CompilationMode mode)
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Modules);
#ifndef __EMSCRIPTEN__
		const size_t worker_count = std::min(schedule.m_all_modules.size(), inputs.Jobs().ValueOr(std::max(1u, std::thread::hardware_concurrency())));
#else
		const size_t worker_count = 1u;
#endif
		timings.SetWorkerCount(worker_count);
		ModuleInterfaceRegistry interfaces(schedule.m_all_modules.size());
		ModuleCompiler compiler(inputs, timings, mode);
		return CompilationExecution::Run(schedule, worker_count, [&build_graph, &interfaces, &compiler, &timings](const ScheduledModule& module, const InterfaceReady& interface_ready)
		{
			CompilationTimings::ModuleTimer timer(timings);
			return CompileGraphModule(build_graph, interfaces, compiler, module.m_file_path, interface_ready);
		});
	}

	static EditorSymbols CollectEditorSymbols(const CompilationSchedule& schedule, std::unordered_map<std::string, CompiledModule>& compiled_modules, const std::string& entry)
	{
		GlobalDeclarations globals;
		ModuleDefinitions entry_definitions;
		for (const std::string& file : schedule.m_tiers | std::views::join)
		{
			CompiledModule& module = compiled_modules.at(file);
			ModuleDefinitions definitions = std::move(module).TakeDefinitions();
			globals.merge(definitions.m_globals);
			for (const auto& [name, origin] : definitions.m_reexports)
			{
				const auto declaration = globals.find(origin + NameSeparator.data() + name);
				if (declaration != globals.end())
				{
					globals.emplace(module.ModuleName() + NameSeparator.data() + name, declaration->second);
				}
			}
			if (file == entry)
			{
				entry_definitions = std::move(definitions);
			}
		}

		std::vector<DefinitionLink> links = std::move(entry_definitions.m_local_links);
		for (auto& [name, reference] : entry_definitions.m_global_references)
		{
			const auto declaration = globals.find(name);
			if (declaration != globals.end())
			{
				links.push_back({ std::move(reference), declaration->second });
			}
		}
		return { std::move(links), std::move(entry_definitions.m_types) };
	}

	static BuildGraphArtifacts CollectArtifacts(const CompilationSchedule& schedule, std::unordered_map<std::string, CompiledModule>& compiled_modules, const std::string* analysis_entry)
	{
		BuildGraphArtifacts artifacts;
		artifacts.m_bytecode_modules.reserve(schedule.m_all_modules.size());
		for (const std::string& file_path : schedule.m_tiers | std::views::join)
		{
			CompiledModule& compiled_module = compiled_modules.at(file_path);
			artifacts.m_warnings.Append(compiled_module.Warnings());
			if (!compiled_module.MarmotIR().empty())
			{
				artifacts.m_marmot_ir.push_back(compiled_module.MarmotIR());
			}
			if (!compiled_module.Ast().empty())
			{
				artifacts.m_ast.push_back(compiled_module.Ast());
			}
			if (analysis_entry == nullptr)
			{
				artifacts.m_bytecode_modules.emplace_back(std::move(compiled_module).TakeBytecode());
			}
		}
		if (analysis_entry != nullptr)
		{
			artifacts.m_symbols = CollectEditorSymbols(schedule, compiled_modules, *analysis_entry);
		}

		return artifacts;
	}

	static MarmotResult::ReportResult<BuildGraphArtifacts> CompileBuildGraph(BuildGraph&& build_graph, const CompilationInputs& inputs, CompilationTimings& timings, const std::string* analysis_entry = nullptr)
	{
		CompilationSchedule schedule = [&]()
		{
			CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Schedule);
			return CompilationSchedule(build_graph);
		}();
		const size_t total_modules = schedule.m_all_modules.size();
		std::unordered_map<std::string, CompiledModule> compiled_modules;
		compiled_modules.reserve(total_modules);
		const CompilationMode mode = analysis_entry == nullptr ? CompilationMode::Build : CompilationMode::Analysis;
		CompilationExecution::Results results = CompileModules(build_graph, schedule, inputs, timings, mode);
		ModuleFailures failures;
		for (auto& [file_path, result] : results)
		{
			if (result.HasValue())
			{
				compiled_modules.emplace(file_path, std::move(result).Value());
			}
			else
			{
				failures.emplace(file_path, std::move(result).Error());
			}
		}
		if (!failures.empty())
		{
			return MarmotCompiler::Failure(CollectFailureReport(schedule, compiled_modules, std::move(failures)));
		}

		return CollectArtifacts(schedule, compiled_modules, analysis_entry);
	}

	static MarmotResult::ReportResult<BuildGraph> Discover(std::string source, const std::string& file_name, const std::vector<std::string>& source_lines, const CompilationInputs& inputs, CompilationTimings& timings)
	{
		MarmotResult::LexerResult tokens = [&]()
		{
			CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Lexing);
			return Lexer(std::move(source), file_name).Lex();
		}();
		if (!tokens)
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerReport(std::move(tokens.Error())));
		}

		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Discovery);
		return ModuleManager(std::move(tokens).Value(), file_name, source_lines, inputs).GenerateBuildGraph()
			.TransformError([](CompilerError error) { return MarmotResult::CompilerReport(std::move(error)); });
	}

	static std::string ResolveEntryModuleName(const BuildGraph& build_graph, const std::string& entry_file_name)
	{
		std::unordered_map<std::string, ModuleDeclaration>::const_iterator entry_decl_it = build_graph.m_module_declarations.find(entry_file_name);
		if (entry_decl_it != build_graph.m_module_declarations.end() && !entry_decl_it->second.ModuleName().empty())
		{
			return entry_decl_it->second.ModuleName();
		}

		return std::filesystem::path(entry_file_name).stem().string();
	}

	static MarmotResult::CompiledProgramReportResult LinkBytecodeModules(BuildGraphArtifacts&& build_graph_artifacts, const std::string& entry_module_name)
	{
		MarmotResult::BytecodeLinkerResult link_result = BytecodeLinker(std::move(build_graph_artifacts.m_bytecode_modules), entry_module_name).Link();
		if (!link_result.HasValue())
		{
			return MarmotCompiler::Failure(MarmotResult::CompilerReport(std::move(build_graph_artifacts.m_warnings), MarmotResult::CompilerDiagnostics(std::move(link_result.Error()))));
		}

		MarmotExecutable linked_executable = std::move(link_result.Value());
		return MarmotResult::CompiledProgram(std::move(linked_executable), MarmotResult::CompilerReport(std::move(build_graph_artifacts.m_warnings)));
	}
}

Compiler::Compiler(std::string&& source_code, std::string&& file_name, CompilationInputs inputs)
	: m_source_code(std::move(source_code)),
	m_file_name(std::move(file_name)),
	m_inputs(std::move(inputs))
{
	MarmotSource::RemoveByteOrderMark(m_source_code);
	std::istringstream stream(m_source_code);
	std::string line;
	while (std::getline(stream, line))
	{
		m_source_lines.push_back(line);
	}

#ifndef __EMSCRIPTEN__
	m_file_name = std::filesystem::weakly_canonical(m_file_name).string();
#else
	if (!m_file_name.empty() && m_file_name[0u] != '/')
	{
		m_file_name = "/" + m_file_name;
	}
#endif
}

MarmotResult::CompiledProgramReportResult Compiler::CompileWithReport()
{
	CompilationTimings timings(m_inputs.EmitsTimings());
	MarmotResult::CompiledProgramReportResult result = CompileWithTimings(timings);
	timings.Print();
	return result;
}

MarmotResult::CompiledProgramReportResult Compiler::CompileWithTimings(CompilationTimings& timings)
{
	CompilationTimings::Timer total(timings, CompilationTimings::Phase::Total);
	MarmotResult::ReportResult<BuildGraph> build_graph_result = Discover(std::move(m_source_code), m_file_name, m_source_lines, m_inputs, timings);
	if (!build_graph_result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(build_graph_result.Error()));
	}

	BuildGraph build_graph = std::move(build_graph_result.Value());
	const std::string entry_module_name = ResolveEntryModuleName(build_graph, m_file_name);

	std::vector<std::string> source_files;
	source_files.reserve(build_graph.m_nodes.size());
	for (const std::pair<const std::string, BuildGraph::BuildNode>& node : build_graph.m_nodes)
	{
		source_files.push_back(node.first);
	}
	std::ranges::sort(source_files);

	MarmotResult::ReportResult<BuildGraphArtifacts> bytecode_result = CompileBuildGraph(std::move(build_graph), m_inputs, timings);
	if (!bytecode_result.HasValue())
	{
		return MarmotCompiler::Failure(std::move(bytecode_result.Error()));
	}

	std::vector<std::string> marmot_ir = std::move(bytecode_result->m_marmot_ir);
	std::vector<std::string> ast = std::move(bytecode_result->m_ast);
	MarmotResult::CompiledProgramReportResult linked = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Linking);
		return LinkBytecodeModules(std::move(bytecode_result).Value(), entry_module_name);
	}();
	if (linked.HasValue())
	{
		linked->m_source_files = std::move(source_files);
		linked->m_marmot_ir = std::move(marmot_ir);
		linked->m_ast = std::move(ast);
	}
	if (linked.HasValue() && !m_inputs.NativeLibraryPolicies().empty())
	{
		std::vector<NativeLibraryImport> libraries = linked->m_executable.GetNativeLibraries();
		for (NativeLibraryImport& library : libraries)
		{
			const std::unordered_map<std::string, NativeLibraryPolicy>::const_iterator policy = m_inputs.NativeLibraryPolicies().find(library.m_name);
			if (policy != m_inputs.NativeLibraryPolicies().end())
			{
				library.m_policy = policy->second;
			}
		}
		linked->m_executable.AttachNativeLibraries(std::move(libraries));
	}

	return linked;
}

EditorAnalysis Compiler::Analyze()
{
	CompilationTimings timings(false);
	MarmotResult::ReportResult<BuildGraphArtifacts> result =
		Discover(std::move(m_source_code), m_file_name, m_source_lines, m_inputs, timings)
			.AndThen([&](BuildGraph&& graph)
			{
				return CompileBuildGraph(std::move(graph), m_inputs, timings, &m_file_name);
			});
	if (!result)
	{
		return { std::move(result.Error()), {} };
	}
	return { MarmotResult::CompilerReport(std::move(result->m_warnings)), std::move(result->m_symbols) };
}

MarmotResult::ExecutableDiagnosticsResult Compiler::Compile()
{
	MarmotResult::CompiledProgramReportResult compile_result = CompileWithReport();
	if (!compile_result.HasValue())
	{
		// Legacy callers still expect an executable-or-errors shape; preserve the
		// new report upstream and narrow only at this adapter boundary.
		return MarmotCompiler::Failure(std::move(compile_result.Error().m_errors));
	}

	return std::move(compile_result.Value()).TakeExecutable();
}

