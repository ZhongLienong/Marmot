#include "Utility/Diagnostics/Diagnostics.h"
#include "Compiler/Constant/Constant.h"
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
#include "Compiler/MidoriIR/Printer/MidoriIRPrinter.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"
#include "Compiler/MidoriIROptimizer/MidoriIROptimizer.h"
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
		CompiledModule::TypeclassMetadataMap m_imported_typeclass_metadata;
		std::unordered_map<std::string, TypeChecker::ClassInfo> m_imported_typeclass_infos;
		CompiledModule::TypeclassMethodMap m_imported_typeclass_methods;
		std::unordered_map<std::string, std::vector<std::string>> m_imported_typeclass_instances;
		std::unordered_map<std::string, std::vector<std::vector<std::shared_ptr<MidoriType>>>> m_imported_typeclass_instance_types;
		TypeChecker::TypeclassInstanceAssociatedTypeBindingMap m_imported_typeclass_instance_associated_type_bindings;
		TypeChecker::TypeclassInstanceConstraintMap m_imported_typeclass_instance_constraints;
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
		std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>>& m_interfaces;
		std::mutex& m_modules_mutex;
		std::mutex& m_print_mutex;
		std::atomic<size_t>& m_completed_modules;
		const std::vector<std::vector<std::string>>& m_tiers;
		size_t m_total_modules;
		bool m_emit_midori_ir;
		bool m_emit_ast;
		CompilationTimings& m_timings;
	};

	struct CompileState;
	using CompileStateResult = MidoriResult::ReportResult<CompileState>;

	struct TypeCheckedModule
	{
		MidoriProgramTree m_ast;
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
		MidoriResult::CompilerWarnings m_warnings;
		ParsedModule m_parsed_module;
		StaticAnalysisResult m_analysis_result;
		MidoriProgramTree m_ast;
		ModuleExportInfo m_export_info;
		BytecodeModule m_bytecode;
		std::optional<LoweredModule> m_lowered;
		std::shared_ptr<const ModuleInterface> m_interface;
		std::unordered_map<std::string, std::string> m_reexports;
		std::string m_midori_ir;
		std::string m_ast_text;
#if MIDORI_ENABLE_OPTIMIZER_STATS
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
		MidoriResult::CompiledModuleReportResult Finalize() &&;
	};

	struct BuildGraphArtifacts
	{
		std::vector<BytecodeModule> m_bytecode_modules;
		std::vector<std::string> m_midori_ir;
		std::vector<std::string> m_ast;
		MidoriResult::CompilerWarnings m_warnings;
	};

	struct CompilerAccess : Compiler
	{
		using Compiler::MergeInstanceEntries;
		using Compiler::MergeInstanceMethods;
		using Compiler::MergeInstanceTypeArgs;
		using Compiler::TypeclassDefinitionsMatch;
	};

	// One instance's type arguments, as text: `Describe<Int>` is "Int", and a
	// multi-parameter class joins them.
	static std::string InstanceSignature(const std::vector<std::shared_ptr<MidoriType>>& type_args)
	{
		std::string signature;
		for (const std::shared_ptr<MidoriType>& type_arg : type_args)
		{
			if (!signature.empty())
			{
				signature.append(", ");
			}
			signature.append(type_arg->ToString());
		}
		return signature;
	}

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
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t, const std::string& optimizer_log)
#else
	static size_t ReportCompiled(CompileEnv& env, const std::string& file_path, size_t)
#endif
	{
		const size_t current_module = env.m_completed_modules.fetch_add(1u) + 1u;
		const std::string short_path = std::filesystem::path(file_path).filename().string();
#if MIDORI_ENABLE_OPTIMIZER_STATS
		if (CompilerDiagnostics::StatisticsEnabled())
		{
			std::lock_guard<std::mutex> lock(env.m_print_mutex);
			std::print(stderr, "MidoriIR optimizer: {}\n{}", short_path, optimizer_log);
		}
#endif

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

	// Which module declared a nominal type. Anything else has no module of its
	// own, so it answers with nothing.
	static std::string DeclaringModuleName(const std::shared_ptr<MidoriType>& type)
	{
		if (type->IsType<MidoriType::StructType>())
		{
			return type->GetType<MidoriType::StructType>().m_module_name;
		}

		if (type->IsType<MidoriType::UnionType>())
		{
			return type->GetType<MidoriType::UnionType>().m_module_name;
		}

		if (type->IsType<MidoriType::NewType>())
		{
			return type->GetType<MidoriType::NewType>().m_module_name;
		}

		return std::string();
	}

	static MidoriResult::Result<ImportContext> BuildImportContext(CompileEnv& env, const BuildGraph::BuildNode& node, const std::string& file_path)
	{
		ImportContext context;
		std::unordered_map<std::string, std::string> imported_typeclass_sources;
		std::vector<const ModuleInterface*> dependency_modules;
		dependency_modules.reserve(node.m_dependencies.size());

		{
			std::lock_guard<std::mutex> lock(env.m_modules_mutex);
			for (const std::string& dep_path : node.m_dependencies)
			{
				dependency_modules.push_back(env.m_interfaces.at(dep_path).get());
			}
		}

		size_t imported_type_count = 0u;
		size_t imported_typeclass_count = 0u;
		size_t imported_generic_function_count = 0u;
		for (const ModuleInterface* dep : dependency_modules)
		{
			imported_type_count += dep->m_type_signatures.size() * 2u;
			imported_typeclass_count += dep->m_typeclass_metadata.size();

			imported_generic_function_count += dep->m_generic_functions.size() * 2u;
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
		// Which module each instance came from, so two instances for the same
		// type can name both.
		std::unordered_map<std::string, std::unordered_map<std::string, std::string>> instance_sources;

		for (const ModuleInterface* dep : dependency_modules)
		{
			const std::string& dep_module_name = dep->m_module_name;
			context.m_imported_symbols[dep_module_name] = dep->m_symbols;
			context.m_imported_type_signatures[dep_module_name] = dep->m_type_signatures;

			for (const auto& [tc_name, metadata] : dep->m_typeclass_metadata)
			{
				std::unordered_map<std::string, CompiledModule::TypeclassMetadata>::iterator existing_it = context.m_imported_typeclass_metadata.find(tc_name);
				if (existing_it != context.m_imported_typeclass_metadata.end())
				{
					if (!CompilerAccess::TypeclassDefinitionsMatch(existing_it->second, metadata))
					{
						return std::unexpected(MidoriError::GenerateModuleErrorWithContext(std::format("Typeclass '{}' is defined in multiple imported modules ('{}' and '{}')", tc_name, imported_typeclass_sources.at(tc_name), dep_module_name), 0, file_path));
					}

					for (const std::vector<std::shared_ptr<MidoriType>>& type_args : metadata.m_declared_instance_type_args)
					{
						const std::string signature = InstanceSignature(type_args);
						const std::unordered_map<std::string, std::string>::const_iterator previous = instance_sources[tc_name].find(signature);
						if (previous != instance_sources[tc_name].cend() && previous->second != dep_module_name)
						{
							return std::unexpected(MidoriError::GenerateModuleErrorWithContext(
								std::format("Typeclass '{}' has two instances for '{}': one in module '{}', one in module '{}'. An instance belongs in the module that declares the class or the module that declares the type.",
									tc_name, signature, previous->second, dep_module_name),
								0,
								file_path));
						}
					}

					CompilerAccess::MergeInstanceMethods(existing_it->second.m_instance_methods, metadata.m_instance_methods);
					CompilerAccess::MergeInstanceEntries(
						existing_it->second.m_instance_associated_type_bindings,
						existing_it->second.m_instance_type_args,
						metadata.m_instance_associated_type_bindings,
						metadata.m_instance_type_args
					);
					CompilerAccess::MergeInstanceEntries(
						existing_it->second.m_instance_constraints,
						existing_it->second.m_instance_type_args,
						metadata.m_instance_constraints,
						metadata.m_instance_type_args
					);
					CompilerAccess::MergeInstanceTypeArgs(existing_it->second.m_instance_type_args, metadata.m_instance_type_args);
				}
				else
				{
					context.m_imported_typeclass_metadata[tc_name] = metadata;
					imported_typeclass_sources[tc_name] = dep_module_name;
				}

				for (const std::vector<std::shared_ptr<MidoriType>>& type_args : metadata.m_declared_instance_type_args)
				{
					instance_sources[tc_name].emplace(InstanceSignature(type_args), dep_module_name);
				}
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

			TypeChecker::ClassInfo info(typeclass_name, std::vector<std::string>(metadata.m_type_param_names), std::move(associated_types), TypeChecker::TypeEnvironment(metadata.m_method_types));
			context.m_imported_typeclass_infos[typeclass_name] = std::move(info);
			context.m_imported_typeclass_methods[typeclass_name] = metadata.m_method_names;
			context.m_imported_typeclass_instances[typeclass_name] = metadata.m_instance_methods;
			context.m_imported_typeclass_instance_types[typeclass_name] = metadata.m_instance_type_args;
			context.m_imported_typeclass_instance_associated_type_bindings[typeclass_name] = metadata.m_instance_associated_type_bindings;
			context.m_imported_typeclass_instance_constraints[typeclass_name] = metadata.m_instance_constraints;
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

	static MidoriResult::DiagnosticsResult<TypeCheckedModule> TypeCheckModule(MidoriProgramTree&& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines, const ImportContext& import_context)
	{
		TypeChecker type_checker(std::move(ast), file_path, module_source_lines, import_context.m_imported_types, import_context.m_imported_typeclass_infos, import_context.m_imported_typeclass_instance_types, import_context.m_imported_typeclass_instance_associated_type_bindings, import_context.m_imported_typeclass_instance_constraints);
		MidoriResult::TypeCheckerResult typecheck_result = type_checker.TypeCheck();
		if (!typecheck_result.has_value())
		{
			return std::unexpected(std::move(typecheck_result.error()));
		}

		return TypeCheckedModule{ std::move(typecheck_result.value()), type_checker.ModuleTypes() };
	}

	static StaticAnalysisResult StaticAnalyzeModule(MidoriProgramTree& ast, const std::string& file_path, const std::vector<std::string>& module_source_lines)
	{
		return StaticAnalyzerManager().Analyze(ast, file_path, module_source_lines);
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
		return ApplyToState<TypeCheckedModule, ApplyTypeCheckedModule>
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

	static LoweringImports MakeLoweringImports(const ImportContext& import_context)
	{
		LoweringImports imports;
		imports.m_generic_functions = import_context.m_imported_generic_functions;
		imports.m_class_methods = import_context.m_imported_typeclass_methods;
		imports.m_class_instances = import_context.m_imported_typeclass_instances;
		imports.m_class_instance_type_args = import_context.m_imported_typeclass_instance_types;
		imports.m_class_instance_associated_types = import_context.m_imported_typeclass_instance_associated_type_bindings;
		return imports;
	}

	// A violation is the compiler's bug, not the program's, so every one is
	// reported and the module stops.
	static CompilerError InvalidMidoriIR(const MidoriIRModule& module, const std::vector<MidoriIRViolation>& violations, std::string_view what, const std::string& file_path)
	{

		const std::string message = violations
			| std::views::transform([](const MidoriIRViolation& violation) { return violation.ToString(); })
			| std::views::join_with('\n')
			| std::ranges::to<std::string>();
		const MidoriIRPrinter printer(module);
		const std::string functions = module.m_functions
			| std::views::filter([&violations](const MidoriIRFunction& function)
			{
				return std::ranges::any_of(violations, [&function](const MidoriIRViolation& violation) { return violation.m_function == function.m_name; });
			})
			| std::views::transform([&printer](const MidoriIRFunction& function) { return printer.PrintFunction(function); })
			| std::views::join_with('\n')
			| std::ranges::to<std::string>();
		return CompilerError::WithFile(CompilerStage::Lowering, std::format("{} produced invalid MidoriIR:\n{}\n\n{}", what, message, functions), file_path, CompilerErrorCode::CompilerInternalError);
	}

	static std::optional<CompilerError> VerifyMidoriIR(const MidoriIRModule& module, std::string_view what, const std::string& file_path)
	{
		const std::vector<MidoriIRViolation> violations = MidoriIRVerifier(module).Verify();
		if (violations.empty())
		{
			return std::nullopt;
		}
		return InvalidMidoriIR(module, violations, what, file_path);
	}

	// Dev builds verify after lowering and after every pass, Release
	// builds once, before the backend.
	static std::optional<CompilerError> OptimizeMidoriIR(MidoriIROptimizer& optimizer, MidoriIRModule& module, const std::string& file_path)
	{
		if (MidoriIROptimizer::VerifiesEachPass())
		{
			std::optional<CompilerError> lowered = VerifyMidoriIR(module, "Lowering", file_path);
			if (lowered.has_value())
			{
				return lowered;
			}
		}
		std::expected<void, MidoriIRPassFailure> optimized = optimizer.Optimize(module);
		if (!optimized.has_value())
		{
			return InvalidMidoriIR(module, optimized.error().m_violations, std::format("The MidoriIR pass {}", optimized.error().m_pass), file_path);
		}
		if (!MidoriIROptimizer::VerifiesEachPass())
		{
			return VerifyMidoriIR(module, "The MidoriIR optimizer", file_path);
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

		MidoriResult::DiagnosticsResult<LoweredModule> lowered = Lowering(state.m_ast, state.m_file_path, state.m_source_lines, state.m_module_name, state.m_export_info.m_export_set, imports).Lower();
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
				std::move(state.m_lowered->m_generic_functions));
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
		MidoriIROptimizer optimizer;
		std::optional<CompilerError> violation = OptimizeMidoriIR(optimizer, state.m_lowered->m_module, state.m_file_path);
		if (violation.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), MidoriResult::CompilerDiagnostics(std::move(violation).value())));
		}
#if MIDORI_ENABLE_OPTIMIZER_STATS
		state.m_optimizer_log = optimizer.Log();
#endif

		if (state.m_env->m_emit_midori_ir)
		{
			state.m_midori_ir = MidoriIRPrinter(state.m_lowered->m_module).Print();
		}
		return state;
	}

	CompileStateResult CompileState::WithBackendBytecode() &&
	{
		CompileState state = std::move(*this);
		MidoriResult::BytecodeBackendResult bytecode_result = BytecodeBackend(state.m_lowered.value(), state.m_file_path, state.m_source_lines).Emit();
		if (!bytecode_result.has_value())
		{
			return std::unexpected(MakeStateErrorReport(std::move(state), std::move(bytecode_result.error())));
		}

		bytecode_result->m_reexports = std::move(state.m_reexports);
		state.m_lowered.reset();
		return ApplyBytecode(std::move(state), std::move(bytecode_result).value());
	}

	MidoriResult::CompiledModuleReportResult CompileState::Finalize() &&
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
		for (const std::pair<const std::string, std::shared_ptr<MidoriType>>& origin_type : state.m_import_context.m_imported_type_signatures.at(origin_module))
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
		const std::unordered_set<std::string>& export_set = state.m_export_info.m_export_set;
		const LoweredModule& lowered_module = state.m_lowered.value();
		const CompiledModule::TypeclassMetadataMap& typeclass_metadata = state.m_parsed_module.m_typeclass_metadata;
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

			return std::unexpected(MakeStateErrorReport(
				std::move(state),
				MidoriResult::CompilerDiagnostics(
				MidoriError::GenerateModuleErrorWithContext(
					CompilerErrorCode::ModuleMissingExportedSymbol,
					message,
					0,
					file_path))));
		}

		return state;
	}

	static MidoriResult::CompiledModuleReportResult BuildCompiledModule(CompileState state)
	{
		CompiledModule compiled_module = CompiledModule(std::move(state.m_interface))
			.WithWarnings(std::move(state.m_warnings).TakeAll())
			.WithBytecode(std::move(state.m_bytecode))
			.WithMidoriIR(std::move(state.m_midori_ir))
			.WithAst(std::move(state.m_ast_text));

		ReportCompiled
		(
			*state.m_env,
			state.m_file_path,
			state.m_tier_idx
#if MIDORI_ENABLE_OPTIMIZER_STATS
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
		MidoriResult::CompiledModuleReportResult Compile(CompileEnv& env, const std::string& file_path, size_t tier_idx, const InterfaceReady& interface_ready) const
		{
			CompilationTimings::ModuleTimer timer(env.m_timings);
			// A worker exception would terminate the process; use the same module
			// diagnostic for worker threads and compilation on the calling thread.
			try
			{
				return MakeCompileState(env, file_path, tier_idx)
					.and_then([&interface_ready](CompileState state) { return RunStages(std::move(state), interface_ready); })
					.and_then
					(
						[](CompileState state) -> MidoriResult::CompiledModuleReportResult
						{
							return std::move(state).Finalize();
						}
					);
			}
			catch (const std::exception& e)
			{
				return MakeInternalErrorResult(file_path, e.what());
			}
			catch (...)
			{
				return MakeInternalErrorResult(file_path, "unknown exception");
			}
		}

	private:
		static MidoriResult::CompiledModuleReportResult MakeInternalErrorResult(const std::string& file_path, const std::string& detail)
		{
			std::string message = "Internal compiler error while compiling '" + file_path + "': " + detail;
			message.push_back(static_cast<char>(10));
			return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, message, CompilerErrorCode::CompilerInternalError)));
		}

		using Stage = CompileStateResult(*)(CompileState);

		static std::string MakeStageFailureMessage(std::string_view stage_name, const std::string& detail)
		{
			std::string message = "internal failure in the ";
			message += stage_name;
			message += " stage: ";
			message += detail;
			return message;
		}

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

		static CompileStateResult RunStages(CompileState state, const InterfaceReady& interface_ready)
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
						return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, MakeStageFailureMessage(stage.m_name, e.what()), CompilerErrorCode::CompilerInternalError)));
					}
					catch (...)
					{
						return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, MakeStageFailureMessage(stage.m_name, "unknown exception"), CompilerErrorCode::CompilerInternalError)));
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
		MidoriResult::CompiledModuleReportResult m_result;
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

	static MidoriResult::CompilerReport CollectFailureReport(const CompilationSchedule& schedule, const CompileEnv& env, const std::string& file_path, MidoriResult::CompilerReport failed_module_report)
	{
		const std::vector<CompilerWarning>& failed_module_warnings = failed_module_report.Warnings().Warnings();
		return MidoriResult::CompilerReport(
			CollectCompiledModuleWarnings(schedule, env.m_compiled_modules, &file_path, &failed_module_warnings),
			std::move(failed_module_report).TakeErrors());
	}

	static MidoriResult::ReportResult<size_t> CompileModulesReadyQueue(CompileEnv& env, ModuleCompiler& module_compiler, CompilationSchedule& schedule, std::optional<size_t> jobs)
	{
		CompilationTimings::Timer timer(env.m_timings, CompilationTimings::Phase::Modules);
		size_t compiled_count = 0u;

#ifndef __EMSCRIPTEN__
		const size_t worker_count = std::min(schedule.m_all_modules.size(), jobs.value_or(std::max(1u, std::thread::hardware_concurrency())));
		env.m_timings.SetWorkerCount(worker_count);
		if (worker_count > 1u)
		{
			ModuleWorkQueue work_queue(env, module_compiler, worker_count);
			work_queue.Enqueue(schedule.InitialReady());

			while (compiled_count < schedule.m_all_modules.size())
			{
				if (work_queue.InFlight() == 0u)
				{
					return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, "No modules are ready to compile. Check for circular dependencies.\n", CompilerErrorCode::CompilerNoModulesReadyToCompile)));
				}

				ModuleEvent event = work_queue.WaitForEvent();
				if (const InterfacePublished* interface = std::get_if<InterfacePublished>(&event))
				{
					work_queue.Enqueue(schedule.InterfaceReady(interface->m_file_path));
					continue;
				}

				CompletedModule completed_module = std::move(std::get<CompletedModule>(event));
				if (!completed_module.m_result.has_value())
				{
					return std::unexpected(CollectFailureReport(schedule, env, completed_module.m_file_path, std::move(completed_module.m_result).error()));
				}

				env.m_compiled_modules.emplace(completed_module.m_file_path, std::move(completed_module.m_result).value());
				compiled_count += 1u;
			}
		}
		else
#else
		(void)jobs;
		env.m_timings.SetWorkerCount(1u);
#endif
		{
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
				MidoriResult::CompiledModuleReportResult result = module_compiler.Compile(env, module.m_file_path, module.m_tier_idx, interface_ready);
				if (!result.has_value())
				{
					return std::unexpected(CollectFailureReport(schedule, env, module.m_file_path, std::move(result).error()));
				}

				env.m_compiled_modules.emplace(module.m_file_path, std::move(result).value());
				compiled_count += 1u;
			}
		}

		if (compiled_count != schedule.m_all_modules.size())
		{
			return std::unexpected(MidoriResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, "Incomplete compilation: some modules never became ready.\n", CompilerErrorCode::CompilerIncompleteCompilationSchedule)));
		}
		return compiled_count;
	}

	static CompileEnv MakeCompileEnv(BuildGraph& build_graph, std::unordered_map<std::string, CompiledModule>& compiled_modules, std::unordered_map<std::string, std::shared_ptr<const ModuleInterface>>& interfaces, std::mutex& modules_mutex, std::mutex& print_mutex, std::atomic<size_t>& completed_modules, const CompilationSchedule& schedule, size_t total_modules, const CompilationInputs& inputs, CompilationTimings& timings)
	{
		return CompileEnv{ build_graph, compiled_modules, interfaces, modules_mutex, print_mutex, completed_modules, schedule.m_tiers, total_modules, inputs.EmitsMidoriIR(), inputs.EmitsAst(), timings };
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
				if (!it->second.MidoriIR().empty())
				{
					artifacts.m_midori_ir.push_back(it->second.MidoriIR());
				}
				if (!it->second.Ast().empty())
				{
					artifacts.m_ast.push_back(it->second.Ast());
				}
				artifacts.m_bytecode_modules.emplace_back(std::move(it->second).TakeBytecode());
			}
		}

		return artifacts;
	}

	static MidoriResult::ReportResult<BuildGraphArtifacts> CompileBuildGraph(BuildGraph&& build_graph, const CompilationInputs& inputs, CompilationTimings& timings)
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
		MidoriResult::ReportResult<size_t> compile_result = CompileModulesReadyQueue(env, module_compiler, schedule, inputs.Jobs());
		if (!compile_result.has_value())
		{
			return std::unexpected(std::move(compile_result.error()));
		}

		MidoriResult::ReportResult<BuildGraphArtifacts> bytecode_result = CollectBytecodeModules(schedule, compiled_modules);
		if (!bytecode_result.has_value())
		{
			return std::unexpected(std::move(bytecode_result.error()));
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

template <typename Entry>
std::vector<Entry>& Compiler::MergeInstanceEntries(
	std::vector<Entry>& target_entries,
	const std::vector<std::vector<std::shared_ptr<MidoriType>>>& target_type_args,
	const std::vector<Entry>& incoming_entries,
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

		if (incoming_idx < incoming_entries.size())
		{
			target_entries.push_back(incoming_entries[incoming_idx]);
		}
		else
		{
			target_entries.emplace_back();
		}
	}

	return target_entries;
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

Compiler::Compiler(std::string&& source_code, std::string&& file_name, CompilationInputs inputs)
	: m_source_code(std::move(source_code)),
	m_file_name(std::move(file_name)),
	m_inputs(std::move(inputs))
{
	MidoriSource::RemoveByteOrderMark(m_source_code);
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
	CompilationTimings timings(m_inputs.EmitsTimings());
	MidoriResult::CompilationResult result = CompileWithTimings(timings);
	timings.Print();
	return result;
}

MidoriResult::CompilationResult Compiler::CompileWithTimings(CompilationTimings& timings)
{
	CompilationTimings::Timer total(timings, CompilationTimings::Phase::Total);
	MidoriResult::LexerResult lex_result = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Lexing);
		return Lexer(std::move(m_source_code), m_file_name).Lex();
	}();
	if (!lex_result.has_value())
	{
		return std::unexpected(MidoriResult::CompilerReport(std::move(lex_result.error())));
	}

	MidoriResult::ModuleManagerResult build_graph_result = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Discovery);
		return ModuleManager(std::move(lex_result.value()), m_file_name, m_source_lines, m_inputs).GenerateBuildGraph();
	}();
	if (!build_graph_result.has_value())
	{
		return std::unexpected(MidoriResult::CompilerReport(std::move(build_graph_result.error())));
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

	MidoriResult::ReportResult<BuildGraphArtifacts> bytecode_result = CompileBuildGraph(std::move(build_graph), m_inputs, timings);
	if (!bytecode_result.has_value())
	{
		return std::unexpected(std::move(bytecode_result.error()));
	}

	std::vector<std::string> midori_ir = std::move(bytecode_result->m_midori_ir);
	std::vector<std::string> ast = std::move(bytecode_result->m_ast);
	MidoriResult::CompilationResult linked = [&]()
	{
		CompilationTimings::Timer timer(timings, CompilationTimings::Phase::Linking);
		return LinkBytecodeModules(std::move(bytecode_result).value(), entry_module_name);
	}();
	if (linked.has_value())
	{
		linked->m_source_files = std::move(source_files);
		linked->m_midori_ir = std::move(midori_ir);
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

