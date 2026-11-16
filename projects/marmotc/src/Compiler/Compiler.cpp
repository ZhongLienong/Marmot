#include "Utility/Diagnostics/Diagnostics.h"
#include "Compiler/Names/SpecialNames.h"
#include "Compiler/Terminal/Terminal.h"
#include "Compiler/Source/Source.h"
#include "Compiler.h"
#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/CompilationTimings/CompilationTimings.h"
#include "Compiler/CompilationSchedule/CompilationSchedule.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/AbstractSyntaxTree/Printer/AbstractSyntaxTreePrinter.h"
#include "Compiler/MarmotIR/Printer/MarmotIRPrinter.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "Compiler/MarmotIROptimizer/MarmotIROptimizer.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <queue>
#include <variant>
#include <filesystem>
#include <mutex>
#include <print>
#include <ranges>
#include <sstream>
#include <exception>
#include <thread>
#include <utility>

using namespace std::string_literals;

namespace
{
	struct ImportContext
	{
		std::unordered_map<std::string, CompiledModule::SymbolTable> m_imported_symbols;
		std::unordered_map<std::string, TypeChecker::TypeEnvironment> m_imported_type_signatures;
		TypeChecker::TypeEnvironment m_imported_types;
		TypeclassMetadataMap m_imported_typeclass_metadata;
		std::unordered_map<std::string, GenericFunctionInfo> m_imported_generic_functions;
		WorkerCrossings m_imported_worker_crossings;
	};

	struct ParsedModule
	{
		MarmotProgramTree m_ast;
		TypeChecker::TypeEnvironment m_type_signatures;
		TypeclassMetadataMap m_typeclass_metadata;
		std::vector<CompilerWarning> m_warnings;
	};

	struct ModuleExportInfo
	{
		CompiledModule::SymbolTable m_symbols;
		std::unordered_set<std::string> m_export_set;
	};

	struct CompileEnv
	{
		BuildGraph& m_build_graph;
		std::unordered_map<std::string, CompiledModule>& m_compiled_modules;
		std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>>& m_interfaces;
		std::mutex& m_modules_mutex;
		std::mutex& m_print_mutex;
		std::atomic<size_t>& m_completed_modules;
		const std::vector<std::vector<std::string>>& m_tiers;
		size_t m_total_modules;
		bool m_emit_marmot_ir;
		bool m_emit_ast;
		CompilationTimings& m_timings;
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
		CompileEnv* m_env = nullptr;
		BuildGraph::BuildNode* m_node = nullptr;
		const ModuleDeclaration* m_module_decl = nullptr;
		size_t m_tier_idx = 0u;
		std::string m_file_path;
		std::string m_module_name;
		ImportContext m_import_context;
		std::vector<std::string> m_source_lines;
		MarmotResult::CompilerWarnings m_warnings;
		ParsedModule m_parsed_module;
		MarmotProgramTree m_ast;
		ModuleExportInfo m_export_info;
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

		CompileStateResult WithImportContext() &&;
		CompileStateResult WithSourceLines() &&;
		CompileStateResult WithParsedModule() &&;
		CompileStateResult WithTypeCheckedAst() &&;
		CompileStateResult WithStaticAnalysis() &&;
		CompileStateResult WithLoweredModule() &&;
		CompileStateResult WithInterface() &&;
		CompileStateResult WithOptimizedModule() &&;
		CompileStateResult WithBackendBytecode() &&;
		MarmotResult::CompiledModuleReportResult Finalize() &&;
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

	static CompileState ApplyImportContext(CompileState state, ImportContext&& import_context)
	{
		state.m_import_context = std::move(import_context);
		return std::move(state);
	}

	static CompileState ApplySourceLines(CompileState state, std::vector<std::string>&& source_lines)
	{
		state.m_source_lines = std::move(source_lines);
		return std::move(state);
	}

	static CompileState ApplyParsedModule(CompileState state, ParsedModule&& parsed_module)
	{
		state.m_warnings.Append(std::move(parsed_module.m_warnings));
		state.m_parsed_module = std::move(parsed_module);
		state.m_ast = std::move(state.m_parsed_module.m_ast);
		return std::move(state);
	}

	// An exported name whose type only inference knows - an un-annotated `def`
	// - has no signature before the module is checked. Now that it is checked,
	// record it, so a module that imports the name can see its type.
	static CompileState ApplyTypeCheckedModule(CompileState state, TypeCheckedModule&& checked)
	{
		if (state.m_module_decl != nullptr)
		{
			for (const ModuleExport& exported : state.m_module_decl->Exports())
			{
				if (state.m_parsed_module.m_type_signatures.contains(exported.m_symbol_name))
				{
					continue;
				}

				const TypeChecker::TypeEnvironment::const_iterator inferred = checked.m_module_types.find(exported.m_symbol_name);
				if (inferred != checked.m_module_types.end())
				{
					state.m_parsed_module.m_type_signatures[exported.m_symbol_name] = inferred->second;
				}
			}
		}

		state.m_ast = std::move(checked.m_ast);
		return std::move(state);
	}

	static CompileState ApplyBytecode(CompileState state, BytecodeModule&& bytecode)
	{
		state.m_bytecode = std::move(bytecode);
		return std::move(state);
	}

#if MARMOT_ENABLE_OPTIMIZER_STATS
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t, const std::string& optimizer_log)
#else
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t)
#endif
	{
		const size_t current_module = env.m_completed_modules.fetch_add(1u) + 1u;
		const std::string short_path = std::filesystem::path(file_path).filename().string();
#if MARMOT_ENABLE_OPTIMIZER_STATS
		if (CompilerDiagnostics::StatisticsEnabled())
		{
			std::lock_guard<std::mutex> lock(env.m_print_mutex);
			std::print(stderr, "MarmotIR optimizer: {}\n{}", short_path, optimizer_log);
		}
#endif

		return current_module;
	}

	static MarmotResult::Result<std::vector<std::string>> LoadModuleSourceLines(const CompileEnv& env, const std::string& file_path)
	{
		return env.m_build_graph.m_nodes.at(file_path).m_source_lines;
	}

	// Which module declared a nominal type. Anything else has no module of its
	// own, so it answers with nothing.
	static std::string DeclaringModuleName(const std::shared_ptr<MarmotType>& type)
	{
		if (type->IsType<MarmotType::StructType>())
		{
			return type->GetType<MarmotType::StructType>().m_module_name;
		}

		if (type->IsType<MarmotType::UnionType>())
		{
			return type->GetType<MarmotType::UnionType>().m_module_name;
		}

		if (type->IsType<MarmotType::NewType>())
		{
			return type->GetType<MarmotType::NewType>().m_module_name;
		}

		return std::string();
	}

	static MarmotResult::Result<ImportContext> BuildImportContext(CompileEnv& env, const BuildGraph::BuildNode& node, const std::string& file_path)
	{
		ImportContext context;
		TypeclassImports typeclass_imports;
		// Each dependency's interface, and the import a conflict between two of
		// them points at.
		std::vector<std::pair<const ModuleInterface*, const Token*>> dependency_modules;
		dependency_modules.reserve(node.m_dependencies.size());

		{
			std::lock_guard<std::mutex> lock(env.m_modules_mutex);
			for (const std::string& dep_path : node.m_dependencies)
			{
				dependency_modules.emplace_back(env.m_interfaces.at(dep_path).get(), &node.m_import_tokens.at(dep_path));
			}
		}

		size_t imported_type_count = 0u;
		size_t imported_generic_function_count = 0u;
		for (const ModuleInterface* dep : dependency_modules | std::views::keys)
		{
			imported_type_count += dep->m_type_signatures.size() * 2u;

			imported_generic_function_count += dep->m_generic_functions.size() * 2u;
		}

		context.m_imported_symbols.reserve(dependency_modules.size());
		context.m_imported_type_signatures.reserve(dependency_modules.size());
		context.m_imported_types.reserve(imported_type_count);
		context.m_imported_generic_functions.reserve(imported_generic_function_count);

		for (const auto& [dep, import_token] : dependency_modules)
		{
			const std::string& dep_module_name = dep->m_module_name;
			context.m_imported_symbols[dep_module_name] = dep->m_symbols;
			context.m_imported_type_signatures[dep_module_name] = dep->m_type_signatures;
			for (const std::pair<const std::string, WorkerCrossing>& crossing : dep->m_worker_crossings)
			{
				context.m_imported_worker_crossings.insert_or_assign(dep_module_name + NameSeparator.data() + crossing.first, crossing.second);
			}

			if (const std::optional<std::string> conflict = typeclass_imports.Import(dep_module_name, dep->m_typeclass_metadata))
			{
				return std::unexpected(CompilerError::WithToken(CompilerStage::Module, *conflict, *import_token, file_path, node.m_source_lines));
			}

			// A type is also known by the module that declared it, which a
			// re-export is not: a constructor is looked up by the type's own
			// identity, and a union's members go with their union.
			std::unordered_map<std::string, std::string> declaring_modules;
			for (const auto& [name, type] : dep->m_type_signatures)
			{
				const std::string declaring_module_name = DeclaringModuleName(type);
				if (!declaring_module_name.empty())
				{
					declaring_modules.emplace(name, declaring_module_name);
				}
			}

			for (const auto& [name, type] : dep->m_type_signatures)
			{
				context.m_imported_types[name] = type;
				context.m_imported_types[dep_module_name + NameSeparator.data() + name] = type;

				const std::unordered_map<std::string, std::string>::const_iterator declaring_it =
					declaring_modules.find(name.substr(0u, name.find(NameSeparator)));
				if (declaring_it != declaring_modules.cend() && declaring_it->second != dep_module_name)
				{
					context.m_imported_types[declaring_it->second + NameSeparator.data() + name] = type;
				}
			}

			// A dependency's own generics go under the module it was reached
			// through and the module that defined it, never under the bare
			// name, which belongs to this module's own. The ones it imported
			// are already `Module::name`, and its generics may call them.
			for (const auto& [name, info] : dep->m_generic_functions)
			{
				if (name.find(NameSeparator) != std::string::npos)
				{
					context.m_imported_generic_functions[name] = info;
					continue;
				}

				context.m_imported_generic_functions[dep_module_name + NameSeparator.data() + name] = info;
				if (!info.m_defining_module.empty() && info.m_defining_module != dep_module_name)
				{
					context.m_imported_generic_functions[info.m_defining_module + NameSeparator.data() + name] = info;
				}
			}
		}

		context.m_imported_typeclass_metadata = std::move(typeclass_imports).TakeMetadata();
		return context;
	}

	static MarmotResult::DiagnosticsResult<ParsedModule> ParseModule(TokenStream&& tokens, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ImportContext& import_context, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl)
	{
		Parser parser(std::move(tokens), file_path, module_source_lines, import_context.m_imported_symbols, import_context.m_imported_type_signatures, use_imports, module_decl, import_context.m_imported_typeclass_metadata);
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
			std::move(type_signatures),
			parser.GetTypeclassMetadata(),
			std::move(warnings)
		};
	}

	static MarmotResult::DiagnosticsResult<TypeCheckedModule> TypeCheckModule(MarmotProgramTree&& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ImportContext& import_context)
	{
		TypeChecker type_checker(std::move(ast), file_path, module_source_lines, import_context.m_imported_types, import_context.m_imported_typeclass_metadata);
		MarmotResult::TypeCheckerResult typecheck_result = type_checker.TypeCheck();
		if (!typecheck_result.has_value())
		{
			return std::unexpected(std::move(typecheck_result.error()));
		}

		return TypeCheckedModule{ std::move(typecheck_result.value()), type_checker.ModuleTypes() };
	}

	static ModuleExportInfo BuildModuleExports(const ModuleDeclaration* module_decl, const TypeclassMetadataMap& typeclass_metadata)
	{
		ModuleExportInfo export_info;
		if (module_decl)
		{
			for (const ModuleExport& exp : module_decl->Exports())
			{
				export_info.m_symbols = std::move(export_info.m_symbols).WithExport(exp.m_symbol_name, exp.m_visibility);
				export_info.m_export_set.insert(exp.m_symbol_name);

				if (typeclass_metadata.contains(exp.m_symbol_name))
				{
					const TypeclassMetadata& tc_metadata = typeclass_metadata.at(exp.m_symbol_name);
					for (const std::string& instance_method : tc_metadata.InstanceMethods())
					{
						export_info.m_symbols = std::move(export_info.m_symbols).WithExport(instance_method, exp.m_visibility);
						export_info.m_export_set.insert(instance_method);
					}
				}
			}
		}

		return export_info;
	}

	static CompileStateResult ValidateExports(CompileState state);
	static MarmotResult::CompiledModuleReportResult BuildCompiledModule(CompileState state);

	CompileStateResult CompileState::WithImportContext() &&
	{
		CompileState state = std::move(*this);
		return ApplyToState<ImportContext, ApplyImportContext>
		(
			BuildImportContext(*state.m_env, *state.m_node, state.m_file_path),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithSourceLines() &&
	{
		CompileState state = std::move(*this);
		return ApplyToState<std::vector<std::string>, ApplySourceLines>
		(
			LoadModuleSourceLines(*state.m_env, state.m_file_path),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithParsedModule() &&
	{
		CompileState state = std::move(*this);
		MarmotResult::DiagnosticsResult<ParsedModule> parse_result =
			ParseModule(std::move(state.m_node->m_tokens), state.m_file_path, state.m_source_lines, state.m_import_context, state.m_node->m_use_imports, state.m_module_decl);
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
			TypeCheckModule(std::move(state.m_ast), state.m_file_path, state.m_source_lines, state.m_import_context),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithStaticAnalysis() &&
	{
		CompileState state = std::move(*this);
		state.m_worker_crossings = FindWorkerCrossings(state.m_ast, state.m_import_context.m_imported_worker_crossings);
		WorkerCrossings visible = state.m_import_context.m_imported_worker_crossings;
		visible.insert(state.m_worker_crossings.cbegin(), state.m_worker_crossings.cend());
		state.m_warnings.Append(StaticAnalyzerManager(visible).Analyze(state.m_ast, state.m_file_path, state.m_source_lines));
		return state;
	}

	static LoweringImports MakeLoweringImports(const ImportContext& import_context)
	{
		LoweringImports imports;
		imports.m_generic_functions = import_context.m_imported_generic_functions;
		imports.m_typeclasses = import_context.m_imported_typeclass_metadata;
		return imports;
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
		state.m_export_info = BuildModuleExports(state.m_module_decl, state.m_parsed_module.m_typeclass_metadata);
		state.m_module_name = state.m_module_decl ? state.m_module_decl->ModuleName() : std::filesystem::path(state.m_file_path).stem().string();

		const LoweringImports imports = MakeLoweringImports(state.m_import_context);
		if (state.m_env->m_emit_ast)
		{
			state.m_ast_text = AbstractSyntaxTreePrinter(state.m_module_name, state.m_ast).Print();
		}

		MarmotResult::DiagnosticsResult<LoweredModule> lowered = Lowering(state.m_ast, state.m_file_path, state.m_source_lines, state.m_module_name, state.m_export_info.m_export_set, imports).Lower();
		if (!lowered.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(lowered.error())));
		}

		state.m_lowered.emplace(std::move(lowered).value());
		return state;
	}

	CompileStateResult CompileState::WithInterface() &&
	{
		return ValidateExports(std::move(*this)).transform([](CompileState state)
		{
			state.m_interface = std::make_shared<const ModuleInterface>(
				state.m_module_name, state.m_file_path, std::move(state.m_export_info.m_symbols),
				std::move(state.m_parsed_module.m_type_signatures), std::move(state.m_parsed_module.m_typeclass_metadata),
				std::move(state.m_lowered->m_generic_functions), std::move(state.m_worker_crossings));
			{
				std::lock_guard<std::mutex> lock(state.m_env->m_modules_mutex);
				state.m_env->m_interfaces.emplace(state.m_file_path, state.m_interface);
			}
			return state;
		});
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

		if (state.m_env->m_emit_marmot_ir)
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

	MarmotResult::CompiledModuleReportResult CompileState::Finalize() &&
	{
		CompilationTimings::Timer timer(m_env->m_timings, CompilationTimings::Phase::Finalization);
		return BuildCompiledModule(std::move(*this));
	}

	// A name a module exports without defining it may be one of its imports.
	// These are the modules that export it to this one.
	static std::vector<std::string> ReexportOrigins(const CompileState& state, const std::string& exported_name)
	{
		std::vector<std::string> origins;
		for (const std::pair<const std::string, CompiledModule::SymbolTable>& imported : state.m_import_context.m_imported_symbols)
		{
			const VisibilityLevel* visibility = imported.second.FindExportVisibility(exported_name);
			if (visibility == nullptr)
			{
				continue;
			}

			if (*visibility == VisibilityLevel::Public || SharesNamespace(state.m_module_name, imported.first))
			{
				origins.push_back(imported.first);
			}
		}

		// `use` already says which module a name came from, so it is also how a
		// facade picks between two imports that export the same one.
		const std::vector<UseImport>::const_iterator used = std::ranges::find_if
		(
			state.m_node->m_use_imports,
			[&exported_name, &origins](const UseImport& use_import)
			{
				return use_import.m_symbol_name == exported_name && std::ranges::find(origins, use_import.m_module_name) != origins.cend();
			}
		);
		if (used != state.m_node->m_use_imports.cend())
		{
			return { used->m_module_name };
		}

		std::ranges::sort(origins);
		return origins;
	}

	// The re-export aliases the original: `Facade::Value` and `Origin::Value`
	// are one symbol, so the type, the generic body and the global all stay the
	// origin's.
	static void AliasReexport(CompileState& state, const std::string& exported_name, const std::string& origin_module)
	{
		state.m_reexports[exported_name] = origin_module;

		// The name itself, and a union's constructors, which are keyed under it.
		const std::string member_prefix = exported_name + NameSeparator.data();
		for (const std::pair<const std::string, std::shared_ptr<MarmotType>>& origin_type : state.m_import_context.m_imported_type_signatures.at(origin_module))
		{
			if (origin_type.first == exported_name || origin_type.first.starts_with(member_prefix))
			{
				state.m_parsed_module.m_type_signatures[origin_type.first] = origin_type.second;
			}
		}

		const std::unordered_map<std::string, GenericFunctionInfo>::const_iterator generic_it =
			state.m_import_context.m_imported_generic_functions.find(origin_module + NameSeparator.data() + exported_name);
		if (generic_it != state.m_import_context.m_imported_generic_functions.cend())
		{
			state.m_lowered->m_generic_functions[exported_name] = generic_it->second;
		}
	}

	static CompileStateResult ValidateExports(CompileState state)
	{
		if (state.m_module_decl == nullptr)
		{
			return state;
		}

		const LoweredModule& lowered_module = state.m_lowered.value();
		const TypeclassMetadataMap& typeclass_metadata = state.m_parsed_module.m_typeclass_metadata;
		const TypeChecker::TypeEnvironment& type_signatures = state.m_parsed_module.m_type_signatures;
		const std::string& module_name = state.m_module_name;
		const std::string& file_path = state.m_file_path;

		std::unordered_set<std::string> defined_exports;
		for (const LoweredExport& exported_symbol : lowered_module.m_exports)
		{
			defined_exports.insert(exported_symbol.m_name);
		}

		for (const auto& [typeclass_name, tc_metadata] : typeclass_metadata)
		{
			defined_exports.insert(typeclass_name);
			for (const std::string& instance_method : tc_metadata.InstanceMethods())
			{
				defined_exports.insert(instance_method);
			}
		}

		// Include type signatures (structs, unions, type aliases) as valid exports
		for (const auto& [type_name, type_ptr] : type_signatures)
		{
			defined_exports.insert(type_name);
		}

		// In the order the export list names them, so the first problem is the
		// one reported.
		for (const ModuleExport& exported : state.m_module_decl->Exports())
		{
			const std::string& exported_name = exported.m_symbol_name;
			if (defined_exports.contains(exported_name))
			{
				continue;
			}

			const std::vector<std::string> origins = ReexportOrigins(state, exported_name);
			if (origins.size() == 1u)
			{
				AliasReexport(state, exported_name, origins.front());
				continue;
			}

			const std::string message = origins.empty()
				? "Symbol '"s + exported_name + "' is exported but not defined in module '"s + module_name + "'"s
				: std::format("Symbol '{}' is re-exported by module '{}', but '{}' and '{}' both export it. Write 'use {}.{{{}}}' to say which one.", exported_name, module_name, origins.front(), origins[1u], origins.front(), exported_name);

			CompilerError error = CompilerError::WithToken(CompilerStage::Module, message, exported.m_token, file_path, state.m_source_lines, std::nullopt, CompilerErrorCode::ModuleMissingExportedSymbol);
			return std::unexpected(MakeStateErrorReport(std::move(state), MarmotResult::CompilerDiagnostics(std::move(error))));
		}

		return state;
	}

	static MarmotResult::CompiledModuleReportResult BuildCompiledModule(CompileState state)
	{
		CompiledModule compiled_module = CompiledModule(std::move(state.m_interface))
			.WithWarnings(std::move(state.m_warnings).TakeAll())
			.WithBytecode(std::move(state.m_bytecode))
			.WithMarmotIR(std::move(state.m_marmot_ir))
			.WithAst(std::move(state.m_ast_text));

		ReportCompiled
		(
			*state.m_env,
			state.m_file_path,
			state.m_tier_idx
#if MARMOT_ENABLE_OPTIMIZER_STATS
			, state.m_optimizer_log
#endif
		);

		return compiled_module;
	}

	static CompileStateResult MakeCompileState(CompileEnv& env, const std::string& file_path, size_t tier_idx)
	{
		BuildGraph::BuildNode& node = env.m_build_graph.m_nodes.at(file_path);
		const ModuleDeclaration* module_decl = env.m_build_graph.m_module_declarations.contains(file_path) ? &env.m_build_graph.m_module_declarations.at(file_path) : nullptr;

		CompileState state;
		state.m_env = &env;
		state.m_file_path = file_path;
		state.m_tier_idx = tier_idx;
		state.m_node = &node;
		state.m_module_decl = module_decl;

		return state;
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

	using InterfaceReady = std::function<void(const std::string&)>;

	class ModuleCompiler
	{
	public:
		MarmotResult::CompiledModuleReportResult Compile(CompileEnv& env, const std::string& file_path, size_t tier_idx, const InterfaceReady& interface_ready) const
		{
			CompilationTimings::ModuleTimer timer(env.m_timings);
			// A worker exception would terminate the process; use the same module
			// diagnostic for worker threads and compilation on the calling thread.
			try
			{
				return MakeCompileState(env, file_path, tier_idx)
					.and_then([&file_path, &interface_ready](CompileState state) { return RunStages(std::move(state), file_path, interface_ready); })
					.and_then
					(
						[](CompileState state) -> MarmotResult::CompiledModuleReportResult
						{
							return std::move(state).Finalize();
						}
					);
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

	private:
		static MarmotResult::CompilerReport InternalError(const std::string& file_path, std::string_view where, std::string_view detail)
		{
			return MarmotResult::CompilerReport(CompilerError::WithFile(CompilerStage::Compiler, std::format("Internal compiler error {}: {}", where, detail), file_path, CompilerErrorCode::CompilerInternalError));
		}

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

		static CompileStateResult RunStages(CompileState state, const std::string& file_path, const InterfaceReady& interface_ready)
		{
			for (const NamedStage& stage : s_pipeline)
			{
				// An exception escaping a stage reaches a worker thread with no
				// handler, so it must be turned into a diagnostic here while the
				// stage that produced it is still known.
				CompileStateResult result = [&]() -> CompileStateResult
				{
					CompilationTimings::Timer timer(state.m_env->m_timings, stage.m_phase);
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
					interface_ready(state.m_file_path);
				}
			}

			return state;
		}
	};

	struct InterfacePublished
	{
		std::string m_file_path;
	};

	struct CompletedModule
	{
		std::string m_file_path;
		MarmotResult::CompiledModuleReportResult m_result;
	};

	using ModuleEvent = std::variant<InterfacePublished, CompletedModule>;

	class ModuleWorkQueue
	{
	public:
		ModuleWorkQueue(CompileEnv& env, const ModuleCompiler& module_compiler, size_t worker_count)
			: m_env(env),
			m_module_compiler(module_compiler)
		{
			m_workers.reserve(worker_count);
			for (size_t i = 0u; i < worker_count; i += 1u)
			{
				m_workers.emplace_back([this]() { WorkerLoop(); });
			}
		}

		ModuleWorkQueue(const ModuleWorkQueue&) = delete;
		ModuleWorkQueue& operator=(const ModuleWorkQueue&) = delete;

		~ModuleWorkQueue()
		{
			Stop();
		}

		void Enqueue(std::vector<ScheduledModule> modules)
		{
			if (modules.empty())
			{
				return;
			}
			const size_t ready_count = modules.size();
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (ScheduledModule& module : modules)
				{
					m_ready.push(std::move(module));
					m_in_flight += 1u;
				}
			}
			if (ready_count == 1u)
			{
				m_ready_cv.notify_one();
			}
			else
			{
				m_ready_cv.notify_all();
			}
		}

		ModuleEvent WaitForEvent()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_events_cv.wait(lock, [this]() { return !m_events.empty(); });
			ModuleEvent event = std::move(m_events.front());
			m_events.pop_front();
			if (std::holds_alternative<CompletedModule>(event))
			{
				m_in_flight -= 1u;
			}
			return event;
		}

		size_t InFlight() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_in_flight;
		}

		void Stop()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_stop = true;
				m_ready = {};
			}
			m_ready_cv.notify_all();
		}

	private:
		void Publish(ModuleEvent event)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_events.push_back(std::move(event));
			}
			m_events_cv.notify_one();
		}

		void WorkerLoop()
		{
			const InterfaceReady interface_ready = [this](const std::string& file_path)
			{
				Publish(InterfacePublished{ file_path });
			};
			while (true)
			{
				std::optional<ScheduledModule> queued_module;
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_ready_cv.wait(lock, [this]() { return m_stop || !m_ready.empty(); });
					if (m_stop)
					{
						return;
					}
					queued_module.emplace(m_ready.top());
					m_ready.pop();
				}

				Publish(CompletedModule{
					queued_module->m_file_path,
					m_module_compiler.Compile(m_env, queued_module->m_file_path, queued_module->m_tier_idx, interface_ready) });
			}
		}

		CompileEnv& m_env;
		const ModuleCompiler& m_module_compiler;
		mutable std::mutex m_mutex;
		std::condition_variable m_ready_cv;
		std::condition_variable m_events_cv;
		std::priority_queue<ScheduledModule> m_ready;
		std::deque<ModuleEvent> m_events;
		size_t m_in_flight = 0u;
		bool m_stop = false;
		// Join workers before destroying the queues, mutex, or condition variables.
		std::vector<std::jthread> m_workers;
	};

	using ModuleFailures = std::unordered_map<std::string, MarmotResult::CompilerReport>;

	static void RecordCompiledModule(CompileEnv& env, ModuleFailures& failures, const std::string& file_path, MarmotResult::CompiledModuleReportResult&& result)
	{
		if (result.has_value())
		{
			env.m_compiled_modules.emplace(file_path, std::move(result).value());
		}
		else
		{
			failures.emplace(file_path, std::move(result).error());
		}
	}

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

	// Every module whose imports all published their interfaces is compiled,
	// even after another module fails, so which modules fail depends on neither
	// the worker count nor which worker finishes first.
	static ModuleFailures CompileModulesReadyQueue(CompileEnv& env, ModuleCompiler& module_compiler, CompilationSchedule& schedule, std::optional<size_t> jobs)
	{
		CompilationTimings::Timer timer(env.m_timings, CompilationTimings::Phase::Modules);
		ModuleFailures failures;

#ifndef __EMSCRIPTEN__
		const size_t worker_count = std::min(schedule.m_all_modules.size(), jobs.value_or(std::max(1u, std::thread::hardware_concurrency())));
		env.m_timings.SetWorkerCount(worker_count);
		if (worker_count > 1u)
		{
			ModuleWorkQueue work_queue(env, module_compiler, worker_count);
			work_queue.Enqueue(schedule.InitialReady());

			while (work_queue.InFlight() > 0u)
			{
				ModuleEvent event = work_queue.WaitForEvent();
				if (const InterfacePublished* interface = std::get_if<InterfacePublished>(&event))
				{
					work_queue.Enqueue(schedule.InterfaceReady(interface->m_file_path));
					continue;
				}

				CompletedModule& completed_module = std::get<CompletedModule>(event);
				RecordCompiledModule(env, failures, completed_module.m_file_path, std::move(completed_module.m_result));
			}
			return failures;
		}
#else
		(void)jobs;
		env.m_timings.SetWorkerCount(1u);
#endif

		std::priority_queue<ScheduledModule> ready;
		for (ScheduledModule& module : schedule.InitialReady())
		{
			ready.push(std::move(module));
		}
		const InterfaceReady interface_ready = [&schedule, &ready](const std::string& file_path)
		{
			for (ScheduledModule& module : schedule.InterfaceReady(file_path))
			{
				ready.push(std::move(module));
			}
		};

		while (!ready.empty())
		{
			const ScheduledModule module = ready.top();
			ready.pop();
			RecordCompiledModule(env, failures, module.m_file_path, module_compiler.Compile(env, module.m_file_path, module.m_tier_idx, interface_ready));
		}
		return failures;
	}

	static CompileEnv MakeCompileEnv(BuildGraph& build_graph, std::unordered_map<std::string, CompiledModule>& compiled_modules, std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>>& interfaces, std::mutex& modules_mutex, std::mutex& print_mutex, std::atomic<size_t>& completed_modules, const CompilationSchedule& schedule, size_t total_modules, const CompilationInputs& inputs, CompilationTimings& timings)
	{
		return CompileEnv{ build_graph, compiled_modules, interfaces, modules_mutex, print_mutex, completed_modules, schedule.m_tiers, total_modules, inputs.EmitsMarmotIR(), inputs.EmitsAst(), timings };
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
		std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>> interfaces;
		interfaces.reserve(total_modules);
		std::mutex modules_mutex;
		std::mutex print_mutex;
		std::atomic<size_t> completed_modules{ 0u };

		CompileEnv env = MakeCompileEnv(build_graph, compiled_modules, interfaces, modules_mutex, print_mutex, completed_modules, schedule, total_modules, inputs, timings);
		ModuleCompiler module_compiler;
		ModuleFailures failures = CompileModulesReadyQueue(env, module_compiler, schedule, inputs.Jobs());
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

