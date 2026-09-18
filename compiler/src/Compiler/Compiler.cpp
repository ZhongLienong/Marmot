#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Constant/Constant.h"
#include "Common/Printer/Printer.h"
#include "Compiler.h"
#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/CodeGenerator/CodeGenerator.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/OptimizerManager/OptimizerManager.h"
#include "Compiler/Parser/Parser.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <exception>
#include <thread>

using namespace std::string_literals;

#if MIDORI_ENABLE_AST_DUMP
#include "Utility/AbstractSyntaxTreePrinter/AbstractSyntaxTreePrinter.h"
#endif

#if MIDORI_ENABLE_DISASSEMBLY
#include "Utility/Disassembler/Disassembler.h"
#endif

namespace
{
	struct ImportContext
	{
		std::unordered_map<std::string, CompiledModule::SymbolTable> m_imported_symbols;
		std::unordered_map<std::string, TypeChecker::TypeEnvironment> m_imported_type_signatures;
		TypeChecker::TypeEnvironment m_imported_types;
		CompiledModule::TypeclassMetadataMap m_imported_typeclass_metadata;
		std::unordered_map<std::string, TypeChecker::ClassInfo> m_imported_typeclass_infos;
		CompiledModule::TypeclassMethodMap m_imported_typeclass_methods;
		std::unordered_map<std::string, std::vector<std::string>> m_imported_typeclass_instances;
		std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>> m_imported_typeclass_instance_types;
		TypeChecker::TypeclassInstanceAssociatedTypeBindingMap m_imported_typeclass_instance_associated_type_bindings;
		std::unordered_map<std::string, GenericFunctionInfo> m_imported_generic_functions;
	};

	struct ParsedModule
	{
		MidoriProgramTree m_ast;
		TypeChecker::TypeEnvironment m_type_signatures;
		CompiledModule::TypeclassMetadataMap m_typeclass_metadata;
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
		std::mutex& m_modules_mutex;
		std::mutex& m_print_mutex;
		std::atomic<size_t>& m_completed_modules;
		const std::vector<std::vector<std::string>>& m_tiers;
		size_t m_total_modules;
	};

	struct CompileState;
	using CompileStateResult = MidoriResult::ReportResult<CompileState>;

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
		MidoriResult::CompilerWarnings m_warnings;
		ParsedModule m_parsed_module;
		StaticAnalysisResult m_analysis_result;
		MidoriProgramTree m_ast;
		ModuleExportInfo m_export_info;
		BytecodeModule m_bytecode;
#if MIDORI_ENABLE_OPTIMIZER_STATS
		OptimizerLog m_optimizer_log;
#endif

		CompileStateResult WithImportContext() &&;
		CompileStateResult WithSourceLines() &&;
		CompileStateResult WithParsedModule() &&;
		CompileStateResult WithTypeCheckedAst() &&;
		CompileStateResult WithStaticAnalysis() &&;
		CompileStateResult WithOptimizedAst() &&;
		CompileStateResult WithBytecode() &&;
		MidoriResult::CompiledModuleReportResult Finalize() &&;
	};

	struct BuildGraphArtifacts
	{
		std::vector<BytecodeModule> m_bytecode_modules;
		MidoriResult::CompilerWarnings m_warnings;
	};

	struct CompilationSchedule
	{
		std::vector<std::vector<std::string>> m_tiers;
		std::vector<std::string> m_all_modules;
		std::unordered_map<std::string, size_t> m_tier_indices;
		std::unordered_map<std::string, size_t> m_remaining_deps;
		std::unordered_map<std::string, std::vector<std::string>> m_dependents;
	};

	struct CompilerAccess : Compiler
	{
		using Compiler::MergeInstanceAssociatedTypeBindings;
		using Compiler::MergeInstanceMethods;
		using Compiler::MergeInstanceTypeArgs;
		using Compiler::TypeclassDefinitionsMatch;
	};

	static MidoriResult::CompilerReport MakeStateErrorReport(CompileState&& state, MidoriResult::CompilerDiagnostics diagnostics)
	{
		return MidoriResult::CompilerReport(std::move(state.m_warnings), std::move(diagnostics));
	}

	template <typename T, CompileState (*Apply)(CompileState, T&&)>
	static CompileStateResult ApplyToState(MidoriResult::Result<T>&& result, CompileState&& state)
	{
		if (!result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), MidoriResult::CompilerDiagnostics(std::move(result.error()))));
		}

		return Apply(std::move(state), std::move(result).value());
	}

	template <typename T, CompileState (*Apply)(CompileState, T&&)>
	static CompileStateResult ApplyToState(MidoriResult::DiagnosticsResult<T>&& result, CompileState&& state)
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

	static CompileState ApplyAst(CompileState state, MidoriProgramTree&& ast)
	{
		state.m_ast = std::move(ast);
		return std::move(state);
	}

	static CompileState ApplyStaticAnalysis(CompileState state, StaticAnalysisResult&& analysis_result)
	{
		state.m_warnings.Append(std::move(analysis_result.m_warnings));
		state.m_analysis_result = std::move(analysis_result);
		return std::move(state);
	}

	static CompileState ApplyBytecode(CompileState state, BytecodeModule&& bytecode)
	{
		state.m_bytecode = std::move(bytecode);
		return std::move(state);
	}

	static CompilationSchedule BuildCompilationSchedule(const BuildGraph& build_graph)
	{
		CompilationSchedule schedule;
		schedule.m_tiers = build_graph.GetCompilationTiers();
		schedule.m_all_modules.reserve(build_graph.m_nodes.size());

		for (const auto& [file_path, _] : build_graph.m_nodes)
		{
			schedule.m_all_modules.push_back(file_path);
		}

		std::ranges::sort(schedule.m_all_modules);

		schedule.m_tier_indices.reserve(schedule.m_all_modules.size());
		for (size_t tier_idx = 0u; tier_idx < schedule.m_tiers.size(); tier_idx += 1u)
		{
			for (const std::string& file_path : schedule.m_tiers[tier_idx])
			{
				schedule.m_tier_indices[file_path] = tier_idx;
			}
		}

		schedule.m_remaining_deps.reserve(schedule.m_all_modules.size());
		schedule.m_dependents.reserve(schedule.m_all_modules.size());
		for (const std::string& file_path : schedule.m_all_modules)
		{
			schedule.m_remaining_deps.emplace(file_path, 0u);
			schedule.m_dependents.emplace(file_path, std::vector<std::string>{});
		}

		for (const std::string& file_path : schedule.m_all_modules)
		{
			const BuildGraph::BuildNode& node = build_graph.m_nodes.at(file_path);
			for (const std::string& dependency : node.m_dependencies)
			{
				if (!build_graph.m_nodes.contains(dependency))
				{
					continue;
				}

				schedule.m_dependents[dependency].push_back(file_path);
				schedule.m_remaining_deps[file_path] += 1u;
			}
		}

		for (auto& [_, dependents] : schedule.m_dependents)
		{
			std::ranges::sort(dependents);
		}

		return schedule;
	}

	// Phase 2 warning policy:
	// - preserve warnings until top-level reporting
	// - report them once in build-schedule order (tier order, then file path order)
	// - do not deduplicate by rendered text
	// - when a module fails after producing warnings, insert those warnings at that
	//   module's schedule position before returning the final error report
	template <typename OnWarnings>
	static void ForEachOrderedWarningGroup(
		const CompilationSchedule& schedule,
		const std::unordered_map<std::string, CompiledModule>& compiled_modules,
		const std::string* failed_module_path,
		const std::vector<CompilerWarning>* failed_module_warnings,
		OnWarnings&& on_warnings)
	{
		bool emitted_failed_module = false;
		const auto emit_group = [&on_warnings](const std::string& file_path, const std::vector<CompilerWarning>& warnings)
		{
			if (!warnings.empty())
			{
				on_warnings(file_path, warnings);
			}
		};

		for (const std::vector<std::string>& tier : schedule.m_tiers)
		{
			for (const std::string& file_path : tier)
			{
				if (failed_module_path != nullptr && file_path == *failed_module_path)
				{
					if (failed_module_warnings != nullptr)
					{
						emit_group(*failed_module_path, *failed_module_warnings);
					}

					emitted_failed_module = true;
				}

				std::unordered_map<std::string, CompiledModule>::const_iterator it = compiled_modules.find(file_path);
				if (it != compiled_modules.end())
				{
					emit_group(file_path, it->second.Warnings());
				}
			}
		}

		if (!emitted_failed_module && failed_module_path != nullptr && failed_module_warnings != nullptr)
		{
			emit_group(*failed_module_path, *failed_module_warnings);
		}
	}

#if MIDORI_ENABLE_OPTIMIZER_STATS
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t tier_idx, const OptimizerLog* optimizer_log)
#else
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t tier_idx)
#endif
	{
		const size_t current_module = env.m_completed_modules.fetch_add(1u) + 1u;
		const std::string short_path = std::filesystem::path(file_path).filename().string();
		if (MidoriBuild::ShouldEmitInternalDiagnostics())
		{
			std::lock_guard<std::mutex> lock(env.m_print_mutex);
			// Show tier info for multi-tier builds, just progress for single tier
			if (env.m_tiers.size() > 1u)
			{
				Printer::PrintLabeled<Printer::Color::BLUE, Printer::Color::WHITE>
				(
					std::format("{}/{}", current_module, env.m_total_modules),
					std::format("Tier {} -> {}\n", tier_idx + 1, short_path)
				);
			}
			else
			{
				Printer::PrintLabeled<Printer::Color::BLUE, Printer::Color::WHITE>
				(
					std::format("{}/{}", current_module, env.m_total_modules),
					std::format("{}\n", short_path)
				);
			}

#if MIDORI_ENABLE_OPTIMIZER_STATS
			if (optimizer_log && optimizer_log->m_enabled)
			{
				Printer::Print<Printer::Color::CYAN>("\n=== Optimization Pass ===\n");
				if (!optimizer_log->m_body.empty())
				{
					Printer::Print<Printer::Color::MAGENTA>(optimizer_log->m_body);
				}
				Printer::Print<Printer::Color::CYAN>("=========================\n\n");
			}
#endif
		}

		return current_module;
	}

	static MidoriResult::CompilerWarnings CollectCompiledModuleWarnings(
		const CompilationSchedule& schedule,
		const std::unordered_map<std::string, CompiledModule>& compiled_modules,
		const std::string* failed_module_path = nullptr,
		const std::vector<CompilerWarning>* failed_module_warnings = nullptr)
	{
		MidoriResult::CompilerWarnings warnings;
		ForEachOrderedWarningGroup(
			schedule,
			compiled_modules,
			failed_module_path,
			failed_module_warnings,
			[&warnings](const std::string&, const std::vector<CompilerWarning>& module_warnings)
			{
				warnings.Append(module_warnings);
			});
		return warnings;
	}

	static MidoriResult::Result<std::vector<std::string>> LoadModuleSourceLines(const CompileEnv& env, const std::string& file_path)
	{
		std::unordered_map<std::string, BuildGraph::BuildNode>::const_iterator node_it = env.m_build_graph.m_nodes.find(file_path);
		if (node_it == env.m_build_graph.m_nodes.end())
		{
			return std::unexpected(MidoriError::GenerateModuleErrorWithContext("Missing build graph node for module: "s + file_path, 0, file_path));
		}

		if (node_it->second.m_source_lines.empty())
		{
			return std::unexpected(MidoriError::GenerateModuleErrorWithContext("Missing source lines for module: "s + file_path, 0, file_path));
		}

		return node_it->second.m_source_lines;
	}

	static MidoriResult::Result<ImportContext> BuildImportContext(CompileEnv& env, const BuildGraph::BuildNode& node, const std::string& file_path)
	{
		ImportContext context;
		std::unordered_map<std::string, std::string> imported_typeclass_sources;
		std::vector<const CompiledModule*> dependency_modules;
		dependency_modules.reserve(node.m_dependencies.size());

		{
			std::lock_guard<std::mutex> lock(env.m_modules_mutex);
			for (const std::string& dep_path : node.m_dependencies)
			{
				dependency_modules.push_back(&env.m_compiled_modules.at(dep_path));
			}
		}

		size_t imported_type_count = 0u;
		size_t imported_typeclass_count = 0u;
		size_t imported_generic_function_count = 0u;
		for (const CompiledModule* dep : dependency_modules)
		{
			imported_type_count += dep->TypeSignatures().size() * 2u;
			imported_typeclass_count += dep->TypeclassMetadataByName().size();

			const std::optional<BytecodeModule>& dep_bytecode = dep->Bytecode();
			if (dep_bytecode.has_value())
			{
				imported_generic_function_count += dep_bytecode.value().m_generic_functions.size() * 2u;
			}
		}

		context.m_imported_symbols.reserve(dependency_modules.size());
		context.m_imported_type_signatures.reserve(dependency_modules.size());
		context.m_imported_types.reserve(imported_type_count);
		context.m_imported_typeclass_metadata.reserve(imported_typeclass_count);
		context.m_imported_typeclass_infos.reserve(imported_typeclass_count);
		context.m_imported_typeclass_methods.reserve(imported_typeclass_count);
		context.m_imported_typeclass_instances.reserve(imported_typeclass_count);
		context.m_imported_typeclass_instance_types.reserve(imported_typeclass_count);
		context.m_imported_generic_functions.reserve(imported_generic_function_count);
		imported_typeclass_sources.reserve(imported_typeclass_count);

		for (const CompiledModule* dep : dependency_modules)
		{
			const std::string& dep_module_name = dep->ModuleName();
			context.m_imported_symbols[dep_module_name] = dep->Symbols();
			context.m_imported_type_signatures[dep_module_name] = dep->TypeSignatures();

			for (const auto& [tc_name, metadata] : dep->TypeclassMetadataByName())
			{
				std::unordered_map<std::string, CompiledModule::TypeclassMetadata>::iterator existing_it = context.m_imported_typeclass_metadata.find(tc_name);
				if (existing_it != context.m_imported_typeclass_metadata.end())
				{
					if (!CompilerAccess::TypeclassDefinitionsMatch(existing_it->second, metadata))
					{
						return std::unexpected(MidoriError::GenerateModuleErrorWithContext(std::format("Typeclass '{}' is defined in multiple imported modules ('{}' and '{}')", tc_name, imported_typeclass_sources.at(tc_name), dep_module_name), 0, file_path));
					}

					CompilerAccess::MergeInstanceMethods(existing_it->second.m_instance_methods, metadata.m_instance_methods);
					CompilerAccess::MergeInstanceAssociatedTypeBindings(
						existing_it->second.m_instance_associated_type_bindings,
						existing_it->second.m_instance_type_args,
						metadata.m_instance_associated_type_bindings,
						metadata.m_instance_type_args
					);
					CompilerAccess::MergeInstanceTypeArgs(existing_it->second.m_instance_type_args, metadata.m_instance_type_args);
				}
				else
				{
					context.m_imported_typeclass_metadata[tc_name] = metadata;
					imported_typeclass_sources[tc_name] = dep_module_name;
				}
			}

			for (const auto& [name, type] : dep->TypeSignatures())
			{
				context.m_imported_types[name] = type;
				context.m_imported_types[dep_module_name + NameSeparator.data() + name] = type;
			}

			const std::optional<BytecodeModule>& dep_bytecode = dep->Bytecode();
			if (dep_bytecode.has_value())
			{
				for (const auto& [name, info] : dep_bytecode.value().m_generic_functions)
				{
					context.m_imported_generic_functions[name] = info;
					context.m_imported_generic_functions[dep_module_name + "::" + name] = info;
				}
			}
		}

		for (const auto& [typeclass_name, metadata] : context.m_imported_typeclass_metadata)
		{
			TypeChecker::AssociatedTypeEnvironment associated_types;
			for (const std::string& associated_type_name : metadata.m_associated_type_names)
			{
				std::vector<std::shared_ptr<MidoriType>> associated_type_args;
				associated_type_args.reserve(metadata.m_type_param_names.size());
				for (const std::string& type_param_name : metadata.m_type_param_names)
				{
					associated_type_args.emplace_back(MidoriType::MakeGenericType(type_param_name));
				}
				associated_types.emplace(associated_type_name, MidoriType::MakeAssociatedType(typeclass_name, associated_type_name, std::move(associated_type_args)));
			}

			TypeChecker::ClassInfo info(typeclass_name, std::vector<std::string>(metadata.m_type_param_names), std::vector<MidoriType::ClassConstraint>{}, std::move(associated_types), TypeChecker::TypeEnvironment(metadata.m_method_types), std::unordered_set<std::string>{});
			context.m_imported_typeclass_infos[typeclass_name] = std::move(info);
			context.m_imported_typeclass_methods[typeclass_name] = metadata.m_method_names;
			context.m_imported_typeclass_instances[typeclass_name] = metadata.m_instance_methods;
			context.m_imported_typeclass_instance_types[typeclass_name] = metadata.m_instance_type_args;
			context.m_imported_typeclass_instance_associated_type_bindings[typeclass_name] = metadata.m_instance_associated_type_bindings;
		}

		return context;
	}

	static MidoriResult::DiagnosticsResult<ParsedModule> ParseModule(TokenStream&& tokens, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ImportContext& import_context, const std::vector<UseImport>& use_imports, const ModuleDeclaration* module_decl)
	{
		Parser parser(std::move(tokens), file_path, module_source_lines, import_context.m_imported_symbols, import_context.m_imported_type_signatures, use_imports, module_decl, import_context.m_imported_typeclass_metadata);
		MidoriResult::ParserResult ast = parser.Parse();
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

	static MidoriResult::DiagnosticsResult<MidoriProgramTree> TypeCheckModule(MidoriProgramTree&& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ImportContext& import_context)
	{
		MidoriResult::TypeCheckerResult typecheck_result = TypeChecker(std::move(ast), file_path, module_source_lines, import_context.m_imported_types, import_context.m_imported_typeclass_infos, import_context.m_imported_typeclass_instance_types, import_context.m_imported_typeclass_instance_associated_type_bindings).TypeCheck();
		if (!typecheck_result.has_value())
		{
			return std::unexpected(std::move(typecheck_result.error()));
		}

		return std::move(typecheck_result.value());
	}

	static StaticAnalysisResult StaticAnalyzeModule(MidoriProgramTree& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines)
	{
		return StaticAnalyzerManager().Analyze(ast, file_path, module_source_lines);
	}

	static MidoriResult::OptimizerResult OptimizeModule(MidoriProgramTree&& ast
#if MIDORI_ENABLE_OPTIMIZER_STATS
		, OptimizerLog* optimizer_log, std::mutex* print_mutex
#endif
	)
	{
#if MIDORI_ENABLE_OPTIMIZER_STATS
		return OptimizerManager(std::move(ast)).Optimize(optimizer_log, print_mutex);
#else
		return OptimizerManager(std::move(ast)).Optimize();
#endif
	}

	static ModuleExportInfo BuildModuleExports(const ModuleDeclaration* module_decl, const CompiledModule::TypeclassMetadataMap& typeclass_metadata)
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
					const CompiledModule::TypeclassMetadata& tc_metadata = typeclass_metadata.at(exp.m_symbol_name);
					for (const std::string& instance_method : tc_metadata.m_instance_methods)
					{
						export_info.m_symbols = std::move(export_info.m_symbols).WithExport(instance_method, exp.m_visibility);
						export_info.m_export_set.insert(instance_method);
					}
				}
			}
		}

		return export_info;
	}

	static MidoriResult::CodeGeneratorResult GenerateModuleBytecode(MidoriProgramTree&& optimized_ast, const std::string& file_path, const std::vector<std::string>& module_source_lines, const std::string& module_name, const std::unordered_set<std::string>& export_set, const ImportContext& import_context)
	{
		return CodeGenerator(std::move(optimized_ast), file_path, module_source_lines, module_name, export_set, import_context.m_imported_typeclass_methods, import_context.m_imported_typeclass_instances, import_context.m_imported_typeclass_instance_types, import_context.m_imported_generic_functions).GenerateModuleBytecode();
	}

	static CompileStateResult ValidateExports(CompileState state);
	static MidoriResult::CompiledModuleReportResult BuildCompiledModule(CompileState state);

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
		MidoriResult::DiagnosticsResult<ParsedModule> parse_result =
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
		return ApplyToState<MidoriProgramTree, ApplyAst>
		(
			TypeCheckModule(std::move(state.m_ast), state.m_file_path, state.m_source_lines, state.m_import_context),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithStaticAnalysis() &&
	{
		CompileState state = std::move(*this);
		StaticAnalysisResult analysis_result = StaticAnalyzeModule(state.m_ast, state.m_file_path, state.m_source_lines);
		if (!analysis_result.m_errors.empty())
		{
			MidoriResult::CompilerReport report(std::move(state.m_warnings), MidoriResult::CompilerDiagnostics(std::move(analysis_result.m_errors)));
			report.AppendWarnings(std::move(analysis_result.m_warnings));
			return std::unexpected(std::move(report));
		}

		return ApplyStaticAnalysis(std::move(state), std::move(analysis_result));
	}

	CompileStateResult CompileState::WithOptimizedAst() &&
	{
		CompileState state = std::move(*this);
		return ApplyToState<MidoriProgramTree, ApplyAst>
		(
			OptimizeModule(std::move(state.m_ast)
#if MIDORI_ENABLE_OPTIMIZER_STATS
				, &state.m_optimizer_log, state.m_env ? &state.m_env->m_print_mutex : nullptr
#endif
			),
			std::move(state)
		);
	}

	CompileStateResult CompileState::WithBytecode() &&
	{
		CompileState state = std::move(*this);
		state.m_export_info = BuildModuleExports(state.m_module_decl, state.m_parsed_module.m_typeclass_metadata);
		state.m_module_name = state.m_module_decl ? state.m_module_decl->ModuleName() : std::filesystem::path(state.m_file_path).stem().string();

		MidoriResult::CodeGeneratorResult bytecode_result =
			GenerateModuleBytecode(std::move(state.m_ast), state.m_file_path, state.m_source_lines, state.m_module_name, state.m_export_info.m_export_set, state.m_import_context);
		if (!bytecode_result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(bytecode_result.error())));
		}

		return ApplyBytecode(std::move(state), std::move(bytecode_result).value());
	}

	MidoriResult::CompiledModuleReportResult CompileState::Finalize() &&
	{
		return ValidateExports(std::move(*this))
			.and_then(BuildCompiledModule);
	}

	static CompileStateResult ValidateExports(CompileState state)
	{
		const std::unordered_set<std::string>& export_set = state.m_export_info.m_export_set;
		const BytecodeModule& module_bytecode = state.m_bytecode;
		const CompiledModule::TypeclassMetadataMap& typeclass_metadata = state.m_parsed_module.m_typeclass_metadata;
		const TypeChecker::TypeEnvironment& type_signatures = state.m_parsed_module.m_type_signatures;
		const std::string& module_name = state.m_module_name;
		const std::string& file_path = state.m_file_path;

		std::unordered_set<std::string> defined_exports;
		for (const BytecodeModule::ExportedSymbol& exported_symbol : module_bytecode.m_exports)
		{
			defined_exports.insert(exported_symbol.m_name);
		}

		for (const auto& [typeclass_name, tc_metadata] : typeclass_metadata)
		{
			defined_exports.insert(typeclass_name);
			for (const std::string& instance_method : tc_metadata.m_instance_methods)
			{
				defined_exports.insert(instance_method);
			}
		}

		// Include type signatures (structs, unions, type aliases) as valid exports
		for (const auto& [type_name, type_ptr] : type_signatures)
		{
			defined_exports.insert(type_name);
		}

		for (const std::string& exported_name : export_set)
		{
			if (!defined_exports.contains(exported_name))
			{
				return std::unexpected(MakeStateErrorReport(
					std::move(state),
					MidoriResult::CompilerDiagnostics(
					MidoriError::GenerateModuleErrorWithContext(
						CompilerErrorCode::ModuleMissingExportedSymbol,
						"Symbol '"s + exported_name + "' is exported but not defined in module '"s + module_name + "'",
						0,
						file_path))));
			}
		}

		return state;
	}

	static MidoriResult::CompiledModuleReportResult BuildCompiledModule(CompileState state)
	{
		CompiledModule compiled_module = CompiledModule(state.m_module_name, state.m_file_path, std::move(state.m_export_info.m_symbols))
			.WithTypeSignatures(std::move(state.m_parsed_module.m_type_signatures))
			.WithTypeclassMetadata(std::move(state.m_parsed_module.m_typeclass_metadata))
			.WithWarnings(std::move(state.m_warnings).TakeAll())
			.WithBytecode(std::move(state.m_bytecode));

		ReportCompiled
		(
			*state.m_env,
			state.m_file_path,
			state.m_tier_idx
#if MIDORI_ENABLE_OPTIMIZER_STATS
			, &state.m_optimizer_log
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

	static CompileStateResult StageOptimizedAst(CompileState state)
	{
		return std::move(state).WithOptimizedAst();
	}

	static CompileStateResult StageBytecode(CompileState state)
	{
		return std::move(state).WithBytecode();
	}

	class ModuleCompiler
	{
	public:
		MidoriResult::CompiledModuleReportResult Compile(CompileEnv& env, const std::string& file_path, size_t tier_idx) const
		{
			return MakeCompileState(env, file_path, tier_idx)
				.and_then(RunStages)
				.and_then
				(
					[](CompileState state) -> MidoriResult::CompiledModuleReportResult
					{
						return std::move(state).Finalize();
					}
				);
		}

	private:
		using Stage = CompileStateResult(*)(CompileState);

		static std::string MakeStageFailureMessage(std::string_view stage_name, const std::string& detail)
		{
			std::string message = "internal failure in the ";
			message += stage_name;
			message += " stage: ";
			message += detail;
			return message;
		}

		static CompileStateResult RunStages(CompileState state)
		{
			static const std::array<Stage, 7u> stages =
			{
				StageImportContext,
				StageSourceLines,
				StageParsedModule,
				StageTypeCheckedAst,
				StageStaticAnalysis,
				StageOptimizedAst,
				StageBytecode
			};

			static const std::array<std::string_view, 7u> stage_names =
			{
				"import context",
				"source lines",
				"parser",
				"type checker",
				"static analyzer",
				"optimizer",
				"code generator"
			};

			for (size_t stage_index = 0u; stage_index < stages.size(); stage_index += 1u)
			{
				// An exception escaping a stage reaches a worker thread with no
				// handler, so it must be turned into a diagnostic here while the
				// stage that produced it is still known.
				CompileStateResult result = [&]() -> CompileStateResult
				{
					try
					{
						return stages[stage_index](std::move(state));
					}
					catch (const std::exception& e)
					{
						return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, MakeStageFailureMessage(stage_names[stage_index], e.what()), CompilerErrorCode::CompilerInternalError)));
					}
					catch (...)
					{
						return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, MakeStageFailureMessage(stage_names[stage_index], "unknown exception"), CompilerErrorCode::CompilerInternalError)));
					}
				}();

				if (!result.has_value())
				{
					return std::unexpected(std::move(result.error()));
				}

				state = std::move(result).value();
			}

			return state;
		}
	};

	struct QueuedModule
	{
		std::string m_file_path;
		size_t m_tier_idx = 0u;
	};

	struct CompletedModule
	{
		std::string m_file_path;
		MidoriResult::CompiledModuleReportResult m_result;
	};

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

		void Enqueue(std::string file_path, size_t tier_idx)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (m_stop)
				{
					return;
				}

				m_ready.emplace_back(QueuedModule{ std::move(file_path), tier_idx });
				m_in_flight += 1u;
			}

			m_ready_cv.notify_one();
		}

		CompletedModule WaitForCompleted()
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_completed_cv.wait
			(
				lock,
				[this]()
				{
					return !m_completed.empty() || m_in_flight == 0u;
				}
			);

			CompletedModule completed_module = std::move(m_completed.front());
			m_completed.pop_front();
			m_in_flight -= 1u;
			return completed_module;
		}

		size_t InFlight() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_in_flight;
		}

		void Stop()
		{
			bool should_notify = false;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				if (!m_stop)
				{
					m_stop = true;
					m_ready.clear();
					should_notify = true;
				}
			}

			if (should_notify)
			{
				m_ready_cv.notify_all();
				m_completed_cv.notify_all();
			}
		}

	private:
		MidoriResult::CompiledModuleReportResult CompileModuleGuarded(const QueuedModule& queued_module) const
		{
			try
			{
				return m_module_compiler.Compile(m_env, queued_module.m_file_path, queued_module.m_tier_idx);
			}
			catch (const std::exception& e)
			{
				return MakeInternalErrorResult(queued_module.m_file_path, e.what());
			}
			catch (...)
			{
				return MakeInternalErrorResult(queued_module.m_file_path, "unknown exception");
			}
		}

		static MidoriResult::CompiledModuleReportResult MakeInternalErrorResult(const std::string& file_path, const std::string& detail)
		{
			std::string message = "Internal compiler error while compiling '" + file_path + "': " + detail;
			message.push_back(static_cast<char>(10));
			return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, message, CompilerErrorCode::CompilerInternalError)));
		}

		void WorkerLoop()
		{
			while (true)
			{
				QueuedModule queued_module;
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_ready_cv.wait
					(
						lock,
						[this]()
						{
							return m_stop || !m_ready.empty();
						}
					);

					if (m_stop && m_ready.empty())
					{
						return;
					}

					queued_module = std::move(m_ready.front());
					m_ready.pop_front();
				}

				// A worker thread has no handler of its own, so an escaping exception
				// would call std::terminate and the process would die by __fastfail
				// with no diagnostic at all -- reported by Windows as
				// STATUS_STACK_BUFFER_OVERRUN, which reads like a stack overflow and
				// is not one. Catching here turns an internal failure into an error
				// that names the module it came from.
				CompletedModule completed_module{ queued_module.m_file_path, CompileModuleGuarded(queued_module) };

				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_completed.emplace_back(std::move(completed_module));
				}

				m_completed_cv.notify_one();
			}
		}

		CompileEnv& m_env;
		const ModuleCompiler& m_module_compiler;
		mutable std::mutex m_mutex;
		std::condition_variable m_ready_cv;
		std::condition_variable m_completed_cv;
		std::deque<QueuedModule> m_ready;
		std::deque<CompletedModule> m_completed;
		std::vector<std::jthread> m_workers;
		size_t m_in_flight = 0u;
		bool m_stop = false;
	};

	static MidoriResult::ReportResult<size_t> CompileModulesReadyQueue(CompileEnv& env, ModuleCompiler& module_compiler, CompilationSchedule& schedule)
	{
		size_t compiled_count = 0u;

#ifndef __EMSCRIPTEN__
		const size_t worker_count = std::max<size_t>
		(
			1u,
			std::min(schedule.m_all_modules.size(), static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency())))
		);
		ModuleWorkQueue work_queue(env, module_compiler, worker_count);

		for (const std::string& file_path : schedule.m_all_modules)
		{
			if (schedule.m_remaining_deps.at(file_path) == 0u)
			{
				work_queue.Enqueue(file_path, schedule.m_tier_indices.at(file_path));
			}
		}

		while (compiled_count < schedule.m_all_modules.size())
		{
			if (work_queue.InFlight() == 0u)
			{
				work_queue.Stop();
				return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, "No modules are ready to compile. Check for circular dependencies.\n", CompilerErrorCode::CompilerNoModulesReadyToCompile)));
			}

			CompletedModule completed_module = work_queue.WaitForCompleted();
			if (!completed_module.m_result.has_value())
			{
				work_queue.Stop();
				MidoriResult::CompilerReport failed_module_report = std::move(completed_module.m_result).error();
				const std::vector<CompilerWarning>& failed_module_warnings = failed_module_report.Warnings().Warnings();
				MidoriResult::CompilerReport report(
					CollectCompiledModuleWarnings(schedule, env.m_compiled_modules, &completed_module.m_file_path, &failed_module_warnings),
					std::move(failed_module_report).TakeErrors());
				return std::unexpected(std::move(report));
			}

			{
				std::lock_guard<std::mutex> lock(env.m_modules_mutex);
				env.m_compiled_modules.emplace(completed_module.m_file_path, std::move(completed_module.m_result).value());
			}

			compiled_count += 1u;

			for (const std::string& dependent : schedule.m_dependents.at(completed_module.m_file_path))
			{
				size_t& remaining = schedule.m_remaining_deps.at(dependent);
				if (remaining > 0u)
				{
					remaining -= 1u;
					if (remaining == 0u)
					{
						work_queue.Enqueue(dependent, schedule.m_tier_indices.at(dependent));
					}
				}
			}
		}

		work_queue.Stop();
#else
		std::deque<std::string> ready;
		for (const std::string& file_path : schedule.m_all_modules)
		{
			if (schedule.m_remaining_deps.at(file_path) == 0u)
			{
				ready.emplace_back(file_path);
			}
		}

		while (!ready.empty())
		{
			std::string file_path = std::move(ready.front());
			ready.pop_front();

			const size_t tier_idx = schedule.m_tier_indices.at(file_path);
			MidoriResult::CompiledModuleReportResult result = module_compiler.Compile(env, file_path, tier_idx);
			if (!result.has_value())
			{
				MidoriResult::CompilerReport failed_module_report = std::move(result).error();
				const std::vector<CompilerWarning>& failed_module_warnings = failed_module_report.Warnings().Warnings();
				MidoriResult::CompilerReport report(
					CollectCompiledModuleWarnings(schedule, env.m_compiled_modules, &file_path, &failed_module_warnings),
					std::move(failed_module_report).TakeErrors());
				return std::unexpected(std::move(report));
			}

			env.m_compiled_modules.emplace(file_path, std::move(result).value());
			compiled_count += 1u;

			for (const std::string& dependent : schedule.m_dependents.at(file_path))
			{
				size_t& remaining = schedule.m_remaining_deps.at(dependent);
				if (remaining > 0u)
				{
					remaining -= 1u;
					if (remaining == 0u)
					{
						ready.emplace_back(dependent);
					}
				}
			}
		}
#endif

		if (compiled_count != schedule.m_all_modules.size())
		{
			return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, "Incomplete compilation: some modules never became ready.\n", CompilerErrorCode::CompilerIncompleteCompilationSchedule)));
		}

		return compiled_count;
	}

	static bool ShouldReportCompilation(const CompilationSchedule& schedule)
	{
		if (schedule.m_tiers.empty())
		{
			return false;
		}

		if (schedule.m_tiers.size() > 1u)
		{
			return true;
		}

		return schedule.m_tiers[0u].size() > 1u;
	}

	static size_t ReportCompilationStart(std::mutex& print_mutex, const CompilationSchedule& schedule, size_t total_modules)
	{
		const size_t tier_count = schedule.m_tiers.size();
		if (MidoriBuild::ShouldEmitInternalDiagnostics())
		{
			std::lock_guard<std::mutex> lock(print_mutex);
			Printer::PrintSeparator(Printer::Color::DARK_GRAY, 60);
			Printer::PrintLabeled<Printer::Color::BRIGHT_CYAN, Printer::Color::WHITE>
			(
				"COMPILING",
				std::format
				(
					"{} module{} in {} tier{}\n",
					total_modules,
					total_modules == 1 ? "" : "s",
					tier_count,
					tier_count == 1u ? "" : "s"
				)
			);
			Printer::PrintSeparator(Printer::Color::DARK_GRAY, 60);
		}
		return total_modules;
	}

	static std::chrono::milliseconds ReportCompilationSuccess(std::mutex& print_mutex, size_t total_modules, std::chrono::milliseconds duration)
	{
		if (MidoriBuild::ShouldEmitInternalDiagnostics())
		{
			std::lock_guard<std::mutex> lock(print_mutex);
			Printer::PrintSeparator(Printer::Color::DARK_GRAY, 60);
			Printer::PrintLabeled<Printer::Color::BRIGHT_GREEN, Printer::Color::WHITE>
			(
				"SUCCESS",
				std::format("Compiled {} module{} in {} ms\n", total_modules, total_modules == 1 ? "" : "s", duration.count())
			);
			Printer::PrintSeparator(Printer::Color::DARK_GRAY, 60);
		}
		return duration;
	}

	static CompileEnv MakeCompileEnv(BuildGraph& build_graph, std::unordered_map<std::string, CompiledModule>& compiled_modules, std::mutex& modules_mutex, std::mutex& print_mutex, std::atomic<size_t>& completed_modules, const CompilationSchedule& schedule, size_t total_modules)
	{
		return CompileEnv{ build_graph, compiled_modules, modules_mutex, print_mutex, completed_modules, schedule.m_tiers, total_modules };
	}

	static MidoriResult::ReportResult<BuildGraphArtifacts> CollectBytecodeModules(const CompilationSchedule& schedule, std::unordered_map<std::string, CompiledModule>& compiled_modules)
	{
		BuildGraphArtifacts artifacts;
		artifacts.m_bytecode_modules.reserve(schedule.m_all_modules.size());
		for (const std::vector<std::string>& tier : schedule.m_tiers)
		{
			for (const std::string& file_path : tier)
			{
				std::unordered_map<std::string, CompiledModule>::iterator it = compiled_modules.find(file_path);
				if (it == compiled_modules.end())
				{
					return std::unexpected(MidoriResult::CompilerReport(
						std::move(artifacts.m_warnings),
						MidoriResult::CompilerDiagnostics(
							CompilerError::WithFile(CompilerStage::Compiler, std::format("Missing compiled module for '{}'\n", file_path), file_path, CompilerErrorCode::CompilerMissingCompiledModule))));
				}

				artifacts.m_warnings.Append(it->second.Warnings());
				artifacts.m_bytecode_modules.emplace_back(std::move(it->second).TakeBytecode());
			}
		}

		return artifacts;
	}

	static MidoriResult::ReportResult<BuildGraphArtifacts> CompileBuildGraph(BuildGraph&& build_graph)
	{
		std::chrono::high_resolution_clock::time_point compile_start = std::chrono::high_resolution_clock::now();
		CompilationSchedule schedule = BuildCompilationSchedule(build_graph);
		const size_t total_modules = schedule.m_all_modules.size();
		std::unordered_map<std::string, CompiledModule> compiled_modules;
		compiled_modules.reserve(total_modules);
		std::mutex modules_mutex;
		std::mutex print_mutex;
		std::atomic<size_t> completed_modules{ 0u };

		const bool should_report = ShouldReportCompilation(schedule);
		if (should_report)
		{
			ReportCompilationStart(print_mutex, schedule, total_modules);
		}

		CompileEnv env = MakeCompileEnv(build_graph, compiled_modules, modules_mutex, print_mutex, completed_modules, schedule, total_modules);
		ModuleCompiler module_compiler;
		MidoriResult::ReportResult<size_t> compile_result = CompileModulesReadyQueue(env, module_compiler, schedule);
		if (!compile_result.has_value())
		{
			return std::unexpected(std::move(compile_result.error()));
		}

		MidoriResult::ReportResult<BuildGraphArtifacts> bytecode_result = CollectBytecodeModules(schedule, compiled_modules);
		if (!bytecode_result.has_value())
		{
			return std::unexpected(std::move(bytecode_result.error()));
		}

		std::chrono::high_resolution_clock::time_point compile_end = std::chrono::high_resolution_clock::now();
		std::chrono::milliseconds compile_duration = std::chrono::duration_cast<std::chrono::milliseconds>(compile_end - compile_start);

		if (should_report)
		{
			ReportCompilationSuccess(print_mutex, total_modules, compile_duration);
		}

		return bytecode_result;
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

	static MidoriResult::CompilationResult LinkBytecodeModules(BuildGraphArtifacts&& build_graph_artifacts, const std::string& entry_module_name)
	{
		MidoriResult::BytecodeLinkerResult link_result = BytecodeLinker(std::move(build_graph_artifacts.m_bytecode_modules), entry_module_name).Link();
		if (!link_result.has_value())
		{
			return std::unexpected(MidoriResult::CompilerReport(std::move(build_graph_artifacts.m_warnings), MidoriResult::CompilerDiagnostics(std::move(link_result.error()))));
		}

		MidoriExecutable linked_executable = std::move(link_result.value());
#if MIDORI_ENABLE_DISASSEMBLY
		if (MidoriBuild::ShouldEmitInternalDiagnostics())
		{
			for (size_t i : std::views::iota(0u, linked_executable.m_procedure_names.size()))
			{
				const std::string& variable_name = linked_executable.m_procedure_names[i];
				Disassembler::DisassembleBytecodeStream(linked_executable, static_cast<int>(i), variable_name.c_str());
			}
		}
#endif
		return MidoriResult::CompiledProgram(std::move(linked_executable), MidoriResult::CompilerReport(std::move(build_graph_artifacts.m_warnings)));
	}
}

std::vector<std::string>& Compiler::MergeInstanceMethods(std::vector<std::string>& target, const std::vector<std::string>& incoming)
{
	for (const std::string& method_name : incoming)
	{
		if (std::ranges::find(target, method_name) == target.end())
		{
			target.emplace_back(method_name);
		}
	}

	return target;
}

bool Compiler::InstanceTypeArgsEqual(const std::vector<std::shared_ptr<MidoriType>>& left, const std::vector<std::shared_ptr<MidoriType>>& right)
{
	if (left.size() != right.size())
	{
		return false;
	}

	for (size_t i = 0u; i < left.size(); i += 1u)
	{
		if (*left[i] != *right[i])
		{
			return false;
		}
	}

	return true;
}

std::vector<std::vector<std::shared_ptr<MidoriType>>>& Compiler::MergeInstanceTypeArgs(std::vector<std::vector<std::shared_ptr<MidoriType>>>& target, const std::vector<std::vector<std::shared_ptr<MidoriType>>>& incoming)
{
	for (const std::vector<std::shared_ptr<MidoriType>>& incoming_args : incoming)
	{
		bool exists = false;
		for (const std::vector<std::shared_ptr<MidoriType>>& existing_args : target)
		{
			if (InstanceTypeArgsEqual(existing_args, incoming_args))
			{
				exists = true;
				break;
			}
		}

		if (!exists)
		{
			target.push_back(incoming_args);
		}
	}

	return target;
}

std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& Compiler::MergeInstanceAssociatedTypeBindings(
	std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& target_bindings,
	const std::vector<std::vector<std::shared_ptr<MidoriType>>>& target_type_args,
	const std::vector<std::unordered_map<std::string, std::shared_ptr<MidoriType>>>& incoming_bindings,
	const std::vector<std::vector<std::shared_ptr<MidoriType>>>& incoming_type_args
)
{
	for (size_t incoming_idx = 0u; incoming_idx < incoming_type_args.size(); incoming_idx += 1u)
	{
		const std::vector<std::shared_ptr<MidoriType>>& incoming_args = incoming_type_args[incoming_idx];

		bool exists = false;
		for (const std::vector<std::shared_ptr<MidoriType>>& existing_args : target_type_args)
		{
			if (InstanceTypeArgsEqual(existing_args, incoming_args))
			{
				exists = true;
				break;
			}
		}

		if (exists)
		{
			continue;
		}

		if (incoming_idx < incoming_bindings.size())
		{
			target_bindings.push_back(incoming_bindings[incoming_idx]);
		}
		else
		{
			target_bindings.emplace_back();
		}
	}

	return target_bindings;
}

bool Compiler::TypeclassDefinitionsMatch(const CompiledModule::TypeclassMetadata& left, const CompiledModule::TypeclassMetadata& right)
{
	if (left.m_method_names != right.m_method_names)
	{
		return false;
	}
	if (left.m_type_param_names != right.m_type_param_names)
	{
		return false;
	}
	if (left.m_associated_type_names != right.m_associated_type_names)
	{
		return false;
	}
	if (left.m_method_types.size() != right.m_method_types.size())
	{
		return false;
	}
	using MethodTypeMap = std::unordered_map<std::string, std::shared_ptr<MidoriType>>;
	for (const auto& [method_name, method_type] : left.m_method_types)
	{
		MethodTypeMap::const_iterator it = right.m_method_types.find(method_name);
		if (it == right.m_method_types.end())
		{
			return false;
		}
		if (*method_type != *it->second)
		{
			return false;
		}
	}
	return true;
}

Compiler::Compiler(std::string&& source_code, std::string&& file_name)
	: m_source_code(std::move(source_code)), 
	m_file_name(std::move(file_name))
{
	std::istringstream stream(m_source_code);
	std::string line;
	while (std::getline(stream, line))
	{
		m_source_lines.push_back(line);
	}

#ifndef __EMSCRIPTEN__
	m_file_name = std::filesystem::absolute(m_file_name).string();
#else
	if (!m_file_name.empty() && m_file_name[0u] != '/')
	{
		m_file_name = "/" + m_file_name;
	}
#endif
}

MidoriResult::CompilationResult Compiler::CompileWithReport()
{
	MidoriResult::LexerResult lex_result = Lexer(std::move(m_source_code), m_file_name).Lex();
	if (!lex_result.has_value())
	{
		return std::unexpected(MidoriResult::CompilerReport(std::move(lex_result.error())));
	}

	MidoriResult::ModuleManagerResult build_graph_result =
		ModuleManager(std::move(lex_result.value()), m_file_name, m_source_lines).GenerateBuildGraph();
	if (!build_graph_result.has_value())
	{
		return std::unexpected(MidoriResult::CompilerReport(std::move(build_graph_result.error())));
	}

	BuildGraph build_graph = std::move(build_graph_result.value());
	const std::string entry_module_name = ResolveEntryModuleName(build_graph, m_file_name);

	MidoriResult::ReportResult<BuildGraphArtifacts> bytecode_result = CompileBuildGraph(std::move(build_graph));
	if (!bytecode_result.has_value())
	{
		return std::unexpected(std::move(bytecode_result.error()));
	}

	return LinkBytecodeModules(std::move(bytecode_result).value(), entry_module_name);
}

MidoriResult::CompilerResult Compiler::Compile()
{
	MidoriResult::CompilationResult compile_result = CompileWithReport();
	if (!compile_result.has_value())
	{
		// Legacy callers still expect an executable-or-errors shape; preserve the
		// new report upstream and narrow only at this adapter boundary.
		return std::unexpected(std::move(compile_result.error()).TakeErrors());
	}

	return std::move(compile_result.value()).TakeExecutable();
}

