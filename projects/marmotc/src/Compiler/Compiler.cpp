#include "Utility/Diagnostics/Diagnostics.h"
#include "Compiler/Terminal/Terminal.h"
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
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <print>
#include <ranges>
#include <sstream>
#include <exception>
#include <thread>
#include <utility>

namespace
{
	struct ParsedModule
	{
		MarmotProgramTree m_ast;
		ModuleExports m_exports;
		std::vector<CompilerWarning> m_warnings;
	};

	struct ModuleCompilationInput
	{
		BuildGraph::BuildNode m_source;
		std::optional<ModuleDeclaration> m_declaration;
		std::vector<std::shared_ptr<const ModuleInterface>> m_dependencies;

		ModuleCompilationInput(BuildGraph::BuildNode source, std::optional<ModuleDeclaration> declaration, std::vector<std::shared_ptr<const ModuleInterface>> dependencies)
			: m_source(std::move(source)),
			m_declaration(std::move(declaration)),
			m_dependencies(std::move(dependencies))
		{
		}
	};

	struct ModuleCompilationOptions
	{
		bool m_emit_marmot_ir;
		bool m_emit_ast;
		CompilationTimings& m_timings;

		ModuleCompilationOptions(const CompilationInputs& inputs, CompilationTimings& timings)
			: m_emit_marmot_ir(inputs.EmitsMarmotIR()),
			m_emit_ast(inputs.EmitsAst()),
			m_timings(timings)
		{
		}
	};

	struct CompileState;
	using CompileStateResult = MarmotResult::ReportResult<CompileState>;

	struct TypeCheckedModule
	{
		MarmotProgramTree m_ast;
		// What checking inferred for the module's own names.
		TypeChecker::TypeEnvironment m_module_types;
	};

	struct CompileState
	{
		ModuleCompilationInput* m_input;
		const ModuleCompilationOptions* m_options;
		const ModuleDeclaration* m_module_decl;
		std::string m_file_path;
		ModuleImports m_imports;
		std::vector<std::string> m_source_lines;
		MarmotResult::CompilerWarnings m_warnings;
		ParsedModule m_parsed_module;
		MarmotProgramTree m_ast;
		BytecodeModule m_bytecode;
		std::optional<LoweredModule> m_lowered;
		std::shared_ptr<const ModuleInterface> m_interface;
		WorkerCrossings m_worker_crossings;
		std::unordered_map<std::string, std::string> m_reexports;
		std::string m_marmot_ir;
		std::string m_ast_text;
#if MARMOT_ENABLE_OPTIMIZER_STATS
		std::string m_optimizer_log;
#endif

		CompileState(ModuleCompilationInput& input, const ModuleCompilationOptions& options)
			: m_input(&input),
			m_options(&options),
			m_module_decl(input.m_declaration.has_value() ? &*input.m_declaration : nullptr),
			m_file_path(input.m_source.m_file_name)
		{
		}

		CompileStateResult WithImportContext() &&;
		CompileStateResult WithSourceLines() &&;
		CompileStateResult WithParsedModule() &&;
		CompileStateResult WithTypeCheckedAst() &&;
		CompileStateResult WithStaticAnalysis() &&;
		CompileStateResult WithLoweredModule() &&;
		CompileStateResult WithInterface() &&;
		CompileStateResult WithOptimizedModule() &&;
		CompileStateResult WithBackendBytecode() &&;
	};

	struct BuildGraphArtifacts
	{
		std::vector<BytecodeModule> m_bytecode_modules;
		std::vector<std::string> m_marmot_ir;
		std::vector<std::string> m_ast;
		MarmotResult::CompilerWarnings m_warnings;
	};

	static MarmotResult::CompilerReport MakeStateErrorReport(CompileState&& state, MarmotResult::CompilerDiagnostics diagnostics)
	{
		return MarmotResult::CompilerReport(std::move(state.m_warnings), std::move(diagnostics));
	}

	template <typename T, CompileState (*Apply)(CompileState, T&&)>
	static CompileStateResult ApplyToState(MarmotResult::Result<T>&& result, CompileState&& state)
	{
		if (!result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), MarmotResult::CompilerDiagnostics(std::move(result.error()))));
		}

		return Apply(std::move(state), std::move(result).value());
	}

	template <typename T, CompileState (*Apply)(CompileState, T&&)>
	static CompileStateResult ApplyToState(MarmotResult::DiagnosticsResult<T>&& result, CompileState&& state)
	{
		if (!result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(result.error())));
		}

		return Apply(std::move(state), std::move(result).value());
	}

	static CompileState ApplyImportContext(CompileState state, ModuleImports&& import_context)
	{
		state.m_imports = std::move(import_context);
		return std::move(state);
	}

	static CompileState ApplyParsedModule(CompileState state, ParsedModule&& parsed_module)
	{
		state.m_warnings.Append(std::move(parsed_module.m_warnings));
		state.m_parsed_module = std::move(parsed_module);
		state.m_ast = std::move(state.m_parsed_module.m_ast);
		return std::move(state);
	}

	static CompileState ApplyTypeCheckedModule(CompileState state, TypeCheckedModule&& checked)
	{
		state.m_parsed_module.m_exports.RecordInferredTypes(checked.m_module_types);
		state.m_ast = std::move(checked.m_ast);
		return std::move(state);
	}

	static CompileState ApplyBytecode(CompileState state, BytecodeModule&& bytecode)
	{
		state.m_bytecode = std::move(bytecode);
		return std::move(state);
	}

	static MarmotResult::Result<ModuleImports> BuildImportContext(const ModuleCompilationInput& input)
	{
		return ModuleImports::Collect(input.m_dependencies).transform_error([&input](ModuleImportConflict conflict)
		{
			const BuildGraph::BuildNode& source = input.m_source;
			return CompilerError::WithToken(CompilerStage::Module, conflict.m_message, source.m_import_tokens.at(conflict.m_dependency.string()), source.m_file_name, source.m_source_lines);
		});
	}

	static MarmotResult::DiagnosticsResult<ParsedModule> ParseModule(TokenStream&& tokens, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ModuleImports& import_context, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl)
	{
		Parser parser(std::move(tokens), file_path, module_source_lines, import_context, use_imports, module_decl);
		MarmotResult::ParserResult ast = parser.Parse();
		if (!ast.has_value())
		{
			return std::unexpected(std::move(ast.error()));
		}

		std::unordered_set<std::string> export_set_for_types;
		if (module_decl)
		{
			for (const ModuleExport& exp : module_decl->Exports())
			{
				export_set_for_types.insert(exp.m_symbol_name);
			}
		}

		TypeChecker::TypeEnvironment type_signatures = TypeChecker::ExtractTypeSignatures(ast.value(), module_decl ? &export_set_for_types : nullptr);

		std::vector<CompilerWarning> warnings = parser.GetWarnings();

		return ParsedModule
		{
			std::move(*ast),
			ModuleExports(module_decl, file_path, std::move(type_signatures), parser.GetTypeclassMetadata()),
			std::move(warnings)
		};
	}

	static MarmotResult::DiagnosticsResult<TypeCheckedModule> TypeCheckModule(MarmotProgramTree&& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ModuleImports& import_context)
	{
		TypeChecker type_checker(std::move(ast), file_path, module_source_lines, import_context.Types(), import_context.Typeclasses());
		MarmotResult::TypeCheckerResult typecheck_result = type_checker.TypeCheck();
		if (!typecheck_result.has_value())
		{
			return std::unexpected(std::move(typecheck_result.error()));
		}

		return TypeCheckedModule{ std::move(typecheck_result.value()), type_checker.ModuleTypes() };
	}

	static MarmotResult::CompiledModuleReportResult BuildCompiledModule(CompileState state);

	CompileStateResult CompileState::WithImportContext() &&
	{
		CompileState state = std::move(*this);
		return ApplyToState<ModuleImports, ApplyImportContext>
		(
			BuildImportContext(*state.m_input),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithSourceLines() &&
	{
		CompileState state = std::move(*this);
		state.m_source_lines = std::move(state.m_input->m_source.m_source_lines);
		return state;
	}

	CompileStateResult CompileState::WithParsedModule() &&
	{
		CompileState state = std::move(*this);
		MarmotResult::DiagnosticsResult<ParsedModule> parse_result =
			ParseModule(std::move(state.m_input->m_source.m_tokens), state.m_file_path, state.m_source_lines, state.m_imports, state.m_input->m_source.m_use_imports, state.m_module_decl);
		if (!parse_result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(parse_result.error())));
		}

		ParsedModule parsed_module = std::move(parse_result).value();
		return ApplyParsedModule(std::move(state), std::move(parsed_module));
	}

	CompileStateResult CompileState::WithTypeCheckedAst() &&
	{
		CompileState state = std::move(*this);
		return ApplyToState<TypeCheckedModule, ApplyTypeCheckedModule>
		(
			TypeCheckModule(std::move(state.m_ast), state.m_file_path, state.m_source_lines, state.m_imports),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithStaticAnalysis() &&
	{
		CompileState state = std::move(*this);
		state.m_worker_crossings = FindWorkerCrossings(state.m_ast, state.m_imports.Crossings());
		WorkerCrossings visible = state.m_imports.Crossings();
		visible.insert(state.m_worker_crossings.cbegin(), state.m_worker_crossings.cend());
		state.m_warnings.Append(StaticAnalyzerManager(visible).Analyze(state.m_ast, state.m_file_path, state.m_source_lines));
		return state;
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

	static std::optional<CompilerError> VerifyMarmotIR(const MarmotIRModule& module, std::string_view what, const std::string& file_path)
	{
		const std::vector<MarmotIRViolation> violations = MarmotIRVerifier(module).Verify();
		if (violations.empty())
		{
			return std::nullopt;
		}
		return InvalidMarmotIR(module, violations, what, file_path);
	}

	// Dev builds verify after lowering and after every pass, Release
	// builds once, before the backend.
	static std::optional<CompilerError> OptimizeMarmotIR(MarmotIROptimizer& optimizer, MarmotIRModule& module, const std::string& file_path)
	{
		if (MarmotIROptimizer::VerifiesEachPass())
		{
			std::optional<CompilerError> lowered = VerifyMarmotIR(module, "Lowering", file_path);
			if (lowered.has_value())
			{
				return lowered;
			}
		}
		std::expected<void, MarmotIRPassFailure> optimized = optimizer.Optimize(module);
		if (!optimized.has_value())
		{
			return InvalidMarmotIR(module, optimized.error().m_violations, std::format("The MarmotIR pass {}", optimized.error().m_pass), file_path);
		}
		if (!MarmotIROptimizer::VerifiesEachPass())
		{
			return VerifyMarmotIR(module, "The MarmotIR optimizer", file_path);
		}
		return std::nullopt;
	}

	CompileStateResult CompileState::WithLoweredModule() &&
	{
		CompileState state = std::move(*this);
		const ModuleExports& exports = state.m_parsed_module.m_exports;
		const LoweringImports imports{ state.m_imports.Generics(), state.m_imports.Typeclasses() };
		if (state.m_options->m_emit_ast)
		{
			state.m_ast_text = AbstractSyntaxTreePrinter(exports.ModuleName(), state.m_ast).Print();
		}

		MarmotResult::DiagnosticsResult<LoweredModule> lowered = Lowering(state.m_ast, state.m_file_path, state.m_source_lines, exports.ModuleName(), exports.Names(), imports).Lower();
		if (!lowered.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(lowered.error())));
		}

		state.m_lowered.emplace(std::move(lowered).value());
		return state;
	}

	CompileStateResult CompileState::WithInterface() &&
	{
		CompileState state = std::move(*this);
		std::expected<PublishedModuleInterface, CompilerError> published = std::move(state.m_parsed_module.m_exports).Publish(
			*state.m_lowered, state.m_imports, state.m_input->m_source.m_use_imports, std::move(state.m_worker_crossings), state.m_source_lines);
		if (!published.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), MarmotResult::CompilerDiagnostics(std::move(published.error()))));
		}
		state.m_interface = std::move(published->m_interface);
		state.m_reexports = std::move(published->m_reexports);
		return state;
	}

	CompileStateResult CompileState::WithOptimizedModule() &&
	{
		CompileState state = std::move(*this);
		MarmotIROptimizer optimizer;
		std::optional<CompilerError> violation = OptimizeMarmotIR(optimizer, state.m_lowered->m_module, state.m_file_path);
		if (violation.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), MarmotResult::CompilerDiagnostics(std::move(violation).value())));
		}
#if MARMOT_ENABLE_OPTIMIZER_STATS
		state.m_optimizer_log = optimizer.Log();
#endif

		if (state.m_options->m_emit_marmot_ir)
		{
			state.m_marmot_ir = MarmotIRPrinter(state.m_lowered->m_module).Print();
		}
		return state;
	}

	CompileStateResult CompileState::WithBackendBytecode() &&
	{
		CompileState state = std::move(*this);
		MarmotResult::BytecodeBackendResult bytecode_result = BytecodeBackend(state.m_lowered.value(), state.m_file_path, state.m_source_lines).Emit();
		if (!bytecode_result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(bytecode_result.error())));
		}

		bytecode_result->m_reexports = std::move(state.m_reexports);
		state.m_lowered.reset();
		return ApplyBytecode(std::move(state), std::move(bytecode_result).value());
	}

	static MarmotResult::CompiledModuleReportResult BuildCompiledModule(CompileState state)
	{
		CompiledModule compiled_module = CompiledModule(std::move(state.m_interface))
			.WithWarnings(std::move(state.m_warnings).TakeAll())
			.WithBytecode(std::move(state.m_bytecode))
			.WithMarmotIR(std::move(state.m_marmot_ir))
			.WithAst(std::move(state.m_ast_text));

		return compiled_module;
	}

	static CompileStateResult StageImportContext(CompileState state)
	{
		return std::move(state).WithImportContext();
	}

	static CompileStateResult StageSourceLines(CompileState state)
	{
		return std::move(state).WithSourceLines();
	}

	static CompileStateResult StageParsedModule(CompileState state)
	{
		return std::move(state).WithParsedModule();
	}

	static CompileStateResult StageTypeCheckedAst(CompileState state)
	{
		return std::move(state).WithTypeCheckedAst();
	}

	static CompileStateResult StageStaticAnalysis(CompileState state)
	{
		return std::move(state).WithStaticAnalysis();
	}

	static CompileStateResult StageLowering(CompileState state)
	{
		return std::move(state).WithLoweredModule();
	}

	static CompileStateResult StageInterface(CompileState state)
	{
		return std::move(state).WithInterface();
	}

	static CompileStateResult StageOptimization(CompileState state)
	{
		return std::move(state).WithOptimizedModule();
	}

	static CompileStateResult StageBytecodeBackend(CompileState state)
	{
		return std::move(state).WithBackendBytecode();
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
		ModuleCompiler(const CompilationInputs& inputs, CompilationTimings& timings)
			: m_options(inputs, timings)
		{
		}

		MarmotResult::CompiledModuleReportResult Compile(ModuleCompilationInput input, const InterfacePublisher& publish) const
		{
			const std::string& file_path = input.m_source.m_file_name;
			return RunStages(CompileState(input, m_options), file_path, publish)
				.and_then([this](CompileState state) -> MarmotResult::CompiledModuleReportResult
				{
					CompilationTimings::Timer finalization(m_options.m_timings, CompilationTimings::Phase::Finalization);
#if MARMOT_ENABLE_OPTIMIZER_STATS
					const std::string& file_path = state.m_input->m_source.m_file_name;
					const std::string optimizer_log = std::move(state.m_optimizer_log);
#endif
					MarmotResult::CompiledModuleReportResult result = BuildCompiledModule(std::move(state));
#if MARMOT_ENABLE_OPTIMIZER_STATS
					if (CompilerDiagnostics::StatisticsEnabled())
					{
						std::lock_guard<std::mutex> lock(m_print_mutex);
						std::print(stderr, "MarmotIR optimizer: {}\n{}", std::filesystem::path(file_path).filename().string(), optimizer_log);
					}
#endif
					return result;
				});
		}

	private:
		using Stage = CompileStateResult(*)(CompileState);

		struct NamedStage
		{
			Stage m_stage;
			std::string_view m_name;
			CompilationTimings::Phase m_phase;
		};

		static constexpr std::array<NamedStage, 9u> s_pipeline =
		{
			NamedStage{ StageImportContext, "import context", CompilationTimings::Phase::Imports },
			NamedStage{ StageSourceLines, "source lines", CompilationTimings::Phase::SourceLines },
			NamedStage{ StageParsedModule, "parser", CompilationTimings::Phase::Parsing },
			NamedStage{ StageTypeCheckedAst, "type checker", CompilationTimings::Phase::TypeChecking },
			NamedStage{ StageStaticAnalysis, "static analyzer", CompilationTimings::Phase::StaticAnalysis },
			NamedStage{ StageLowering, "lowering", CompilationTimings::Phase::Lowering },
			NamedStage{ StageInterface, "module interface", CompilationTimings::Phase::Interface },
			NamedStage{ StageOptimization, "optimization", CompilationTimings::Phase::Optimization },
			NamedStage{ StageBytecodeBackend, "bytecode backend", CompilationTimings::Phase::Backend }
		};

		static CompileStateResult RunStages(CompileState state, const std::string& file_path, const InterfacePublisher& publish)
		{
			for (const NamedStage& stage : s_pipeline)
			{
				// An exception escaping a stage reaches a worker thread with no
				// handler, so it must be turned into a diagnostic here while the
				// stage that produced it is still known.
				CompileStateResult result = [&]() -> CompileStateResult
				{
					CompilationTimings::Timer timer(state.m_options->m_timings, stage.m_phase);
					try
					{
						return stage.m_stage(std::move(state));
					}
					catch (const std::exception& e)
					{
						return std::unexpected(InternalError(file_path, std::format("in the {} stage", stage.m_name), e.what()));
					}
					catch (...)
					{
						return std::unexpected(InternalError(file_path, std::format("in the {} stage", stage.m_name), "unknown exception"));
					}
				}();

				if (!result.has_value())
				{
					return std::unexpected(std::move(result.error()));
				}

				state = std::move(result).value();
				if (stage.m_stage == StageInterface)
				{
					publish(state.m_interface);
				}
			}

			return state;
		}

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
			std::optional<ModuleDeclaration> declaration;
			const std::unordered_map<std::string, ModuleDeclaration>::iterator declared = build_graph.m_module_declarations.find(file_path);
			if (declared != build_graph.m_module_declarations.end())
			{
				declaration.emplace(std::move(declared->second));
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
			return std::unexpected(InternalError(file_path, "while compiling the module", e.what()));
		}
		catch (...)
		{
			return std::unexpected(InternalError(file_path, "while compiling the module", "unknown exception"));
		}
	}

	// Keep compiling eligible modules after failures so worker timing cannot
	// change which diagnostics are collected.
	static CompilationExecution::Results CompileModules(BuildGraph& build_graph, CompilationSchedule& schedule, const CompilationInputs& inputs, CompilationTimings& timings)
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Modules);
#ifndef __EMSCRIPTEN__
		const size_t worker_count = std::min(schedule.m_all_modules.size(), inputs.Jobs().value_or(std::max(1u, std::thread::hardware_concurrency())));
#else
		const size_t worker_count = 1u;
#endif
		timings.SetWorkerCount(worker_count);
		ModuleInterfaceRegistry interfaces(schedule.m_all_modules.size());
		ModuleCompiler compiler(inputs, timings);
		return CompilationExecution::Run(schedule, worker_count, [&build_graph, &interfaces, &compiler, &timings](const ScheduledModule& module, const InterfaceReady& interface_ready)
		{
			CompilationTimings::ModuleTimer timer(timings);
			return CompileGraphModule(build_graph, interfaces, compiler, module.m_file_path, interface_ready);
		});
	}

	static BuildGraphArtifacts CollectBytecodeModules(const CompilationSchedule& schedule, std::unordered_map<std::string, CompiledModule>& compiled_modules)
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
			artifacts.m_bytecode_modules.emplace_back(std::move(compiled_module).TakeBytecode());
		}

		return artifacts;
	}

	static MarmotResult::ReportResult<BuildGraphArtifacts> CompileBuildGraph(BuildGraph&& build_graph, const CompilationInputs& inputs, CompilationTimings& timings)
	{
		CompilationSchedule schedule = [&]()
		{
			CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Schedule);
			return CompilationSchedule(build_graph);
		}();
		const size_t total_modules = schedule.m_all_modules.size();
		std::unordered_map<std::string, CompiledModule> compiled_modules;
		compiled_modules.reserve(total_modules);
		CompilationExecution::Results results = CompileModules(build_graph, schedule, inputs, timings);
		ModuleFailures failures;
		for (auto& [file_path, result] : results)
		{
			if (result.has_value())
			{
				compiled_modules.emplace(file_path, std::move(result).value());
			}
			else
			{
				failures.emplace(file_path, std::move(result).error());
			}
		}
		if (!failures.empty())
		{
			return std::unexpected(CollectFailureReport(schedule, compiled_modules, std::move(failures)));
		}

		return CollectBytecodeModules(schedule, compiled_modules);
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

	static MarmotResult::CompilationResult LinkBytecodeModules(BuildGraphArtifacts&& build_graph_artifacts, const std::string& entry_module_name)
	{
		MarmotResult::BytecodeLinkerResult link_result = BytecodeLinker(std::move(build_graph_artifacts.m_bytecode_modules), entry_module_name).Link();
		if (!link_result.has_value())
		{
			return std::unexpected(MarmotResult::CompilerReport(std::move(build_graph_artifacts.m_warnings), MarmotResult::CompilerDiagnostics(std::move(link_result.error()))));
		}

		MarmotExecutable linked_executable = std::move(link_result.value());
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

MarmotResult::CompilationResult Compiler::CompileWithReport()
{
	CompilationTimings timings(m_inputs.EmitsTimings());
	MarmotResult::CompilationResult result = CompileWithTimings(timings);
	timings.Print();
	return result;
}

MarmotResult::CompilationResult Compiler::CompileWithTimings(CompilationTimings& timings)
{
	CompilationTimings::Timer total(timings, CompilationTimings::Phase::Total);
	MarmotResult::LexerResult lex_result = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Lexing);
		return Lexer(std::move(m_source_code), m_file_name).Lex();
	}();
	if (!lex_result.has_value())
	{
		return std::unexpected(MarmotResult::CompilerReport(std::move(lex_result.error())));
	}

	MarmotResult::ModuleManagerResult build_graph_result = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Discovery);
		return ModuleManager(std::move(lex_result.value()), m_file_name, m_source_lines, m_inputs).GenerateBuildGraph();
	}();
	if (!build_graph_result.has_value())
	{
		return std::unexpected(MarmotResult::CompilerReport(std::move(build_graph_result.error())));
	}

	BuildGraph build_graph = std::move(build_graph_result.value());
	const std::string entry_module_name = ResolveEntryModuleName(build_graph, m_file_name);

	std::vector<std::string> source_files;
	source_files.reserve(build_graph.m_nodes.size());
	for (const std::pair<const std::string, BuildGraph::BuildNode>& node : build_graph.m_nodes)
	{
		source_files.push_back(node.first);
	}
	std::ranges::sort(source_files);

	MarmotResult::ReportResult<BuildGraphArtifacts> bytecode_result = CompileBuildGraph(std::move(build_graph), m_inputs, timings);
	if (!bytecode_result.has_value())
	{
		return std::unexpected(std::move(bytecode_result.error()));
	}

	std::vector<std::string> marmot_ir = std::move(bytecode_result->m_marmot_ir);
	std::vector<std::string> ast = std::move(bytecode_result->m_ast);
	MarmotResult::CompilationResult linked = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Linking);
		return LinkBytecodeModules(std::move(bytecode_result).value(), entry_module_name);
	}();
	if (linked.has_value())
	{
		linked->m_source_files = std::move(source_files);
		linked->m_marmot_ir = std::move(marmot_ir);
		linked->m_ast = std::move(ast);
	}
	if (linked.has_value() && !m_inputs.NativeLibraryPolicies().empty())
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

MarmotResult::CompilerResult Compiler::Compile()
{
	MarmotResult::CompilationResult compile_result = CompileWithReport();
	if (!compile_result.has_value())
	{
		// Legacy callers still expect an executable-or-errors shape; preserve the
		// new report upstream and narrow only at this adapter boundary.
		return std::unexpected(std::move(compile_result.error().m_errors));
	}

	return std::move(compile_result.value()).TakeExecutable();
}

