#include "Utility/CLI/CLI.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <iterator>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Utility/Diagnostics/Diagnostics.h"
#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Compiler/Json/Json.h"
#include "Compiler/Json/JsonReader.h"
#include "Support/Terminal/Terminal.h"
#include "Utility/BuildPlan/BuildPlan.h"
#include "Utility/Driver/MarmotDriver.h"
#include "Utility/Formatter/Formatter.h"
#include "Support/Expected.h"
#include "Support/Optional.h"

#include <CLI/CLI.hpp>

namespace
{
	enum class OutputFormat
	{
		Text,
		Json
	};

	enum class CommandKind
	{
		Overview,
		Version,
		Check,
		Analyze,
		Build,
		Fmt
	};

	struct Invocation
	{
		CommandKind m_kind = CommandKind::Overview;
		OutputFormat m_format = OutputFormat::Text;
		bool m_show_help = false;
		std::filesystem::path m_source_file;
		// Set by --plan: the plan's inputs replace all discovery, and its entry
		// becomes m_source_file.
		MarmotCompiler::Optional<CompilationInputs> m_plan_inputs = MarmotCompiler::None;
		std::vector<std::filesystem::path> m_fmt_paths;
		bool m_fmt_write = false;
		bool m_fmt_check = false;
		bool m_embed_sources = false;
		// build: where the .mmc goes (-o), where to list the files it was built
		// from (--deps), and whether to leave out the summary.
		MarmotCompiler::Optional<std::filesystem::path> m_output_path = MarmotCompiler::None;
		MarmotCompiler::Optional<std::filesystem::path> m_deps_path = MarmotCompiler::None;
		bool m_quiet = false;
		// check and build: --emit-ir and --emit-ast, which print each
		// module's MarmotIR and checked syntax tree. They need text output.
		bool m_emit_ir = false;
		bool m_emit_ast = false;
		bool m_optimizer_stats = false;
		MarmotCompiler::Optional<size_t> m_jobs;
		bool m_timings = false;
	};

	using ParseResult = MarmotCompiler::Expected<Invocation, std::string>;
	using CommandHandler = int(*)(const Invocation&);

	struct CommandSpec
	{
		std::string_view m_name;
		std::string_view m_summary;
		CommandKind m_kind;
		CommandHandler m_handler;
	};

	const std::vector<CommandSpec>& CommandTable();

	void PrintCliError(std::string_view message)
	{
		std::print(stderr, "{}{}{}\n", MarmotTerminal::Code(MarmotTerminal::Color::RED), message, MarmotTerminal::Code(MarmotTerminal::Color::RESET));
	}

	[[nodiscard]] std::string CommandHelp(std::string_view command_name)
	{
		if (command_name == "check")
		{
			return
				"Usage: marmotc check (<file> | --plan <plan.json>) [--format json]\n"
				"Type-check a Marmot source file without executing it.\n"
				"With --plan, check the plan's entry from exactly the plan's inputs.\n\n"
				"Options: --emit-ast (checked AST), --emit-ir (optimized IR).\n"
				"         --jobs N (maximum workers; default: hardware concurrency).\n"
				"         --timings (stage times and worker utilization on stderr).\n"
#if MARMOT_ENABLE_OPTIMIZER_STATS
				"         --optimizer-stats (pass statistics on stderr).\n"
#endif

				"Examples:\n"
				"  marmotc check src/Main.mmt\n"
				"  marmotc check src/Main.mmt --format json\n"
				"  marmotc check --plan build/plan.json\n";
		}

		if (command_name == "build")
		{
			return
				"Usage: marmotc build (<file> | --plan <plan.json>) [-o <file.mmc>] [--deps <file>]\n"
				"                     [--embed-sources] [--quiet] [--format json]\n"
				"Compile a Marmot source file to a .mmc program, which marmotvm runs.\n"
				"The .mmc goes beside the source file, or to -o.\n"
				"With --embed-sources, embed source file content in the artifact for\n"
				"richer runtime error reporting without the original .mmt on disk.\n"
				"With --plan, build the plan's entry from exactly the plan's inputs.\n"
				"With --quiet, print only diagnostics.\n"
				"With --deps, list every file the program was built from, one per line:\n"
				"what the build depends on.\n\n"
				"Options: --emit-ast (checked AST), --emit-ir (optimized IR).\n"
				"         --jobs N (maximum workers; default: hardware concurrency).\n"
				"         --timings (stage times and worker utilization on stderr).\n"
#if MARMOT_ENABLE_OPTIMIZER_STATS
				"         --optimizer-stats (pass statistics on stderr).\n"
#endif

				"Examples:\n"
				"  marmotc build src/Main.mmt\n"
				"  marmotc build src/Main.mmt -o target/Main.mmc\n"
				"  marmotc build src/Main.mmt --embed-sources\n"
				"  marmotc build src/Main.mmt --format json\n"
				"  marmotc build --plan build/plan.json\n";
		}

		if (command_name == "analyze")
		{
			return
				"Usage: marmotc analyze < request.json\n"
				"Read a build plan and editor buffers as JSON and return diagnostics as JSON.\n"
				"The request has a 'plan' with an entry and a 'sources' object mapping\n"
				"absolute file paths to their current text.\n";
		}

		if (command_name == "fmt")
		{
			return
				"Usage: marmotc fmt <file|dir>... [--write|-w] [--check] [--format json]\n"
				"Format Marmot source files using the canonical CLI style.\n\n"
				"Examples:\n"
				"  marmotc fmt src/Main.mmt\n"
				"  marmotc fmt src -w\n"
				"  marmotc fmt src test --check\n";
		}

		return {};
	}

	[[nodiscard]] std::string GeneralHelp()
	{
		return
			"Marmot CLI\n\n"
			"Usage:\n"
			"  marmotc <command> [options]\n\n"
			"Commands:\n"
			"  check    Type-check a source file without executing it\n"
			"  analyze  Analyze editor buffers and report diagnostics as JSON\n"
			"  build    Compile a source file to a .mmc program\n"
			"  fmt      Format one file or a directory of .mmt files\n"
			"  help     Show general or per-command help\n\n"
			"Global flags:\n"
			"  --help      Show help\n"
			"  --version   Show the Marmot version\n\n"
			"Examples:\n"
			"  marmotc fmt src -w\n"
			"  marmotc check src/Main.mmt --format json\n"
			"  marmotc build src/Main.mmt -o target/Main.mmc\n"
			"  marmotvm run target/Main.mmc\n\n"
			"marmotc compiles what it is given: a file, with <Name> imports found through\n"
			"MARMOT_PATH, or a build plan. It never runs a program: marmotvm runs what it\n"
			"builds. Projects, packages, running and testing are the marmot tool's job:\n"
			"  marmot run, marmot test, marmot install, marmot init\n";
	}

	[[nodiscard]] int EditDistance(std::string_view left, std::string_view right)
	{
		std::vector<int> previous(right.size() + 1u);
		std::vector<int> current(right.size() + 1u);
		for (size_t index = 0u; index <= right.size(); index += 1u)
		{
			previous[index] = static_cast<int>(index);
		}

		for (size_t left_index = 0u; left_index < left.size(); left_index += 1u)
		{
			current[0u] = static_cast<int>(left_index + 1u);
			for (size_t right_index = 0u; right_index < right.size(); right_index += 1u)
			{
				const int substitution_cost = left[left_index] == right[right_index] ? 0 : 1;
				current[right_index + 1u] = std::min(
					std::min(current[right_index] + 1, previous[right_index + 1u] + 1),
					previous[right_index] + substitution_cost);
			}
			previous = current;
		}

		return previous[right.size()];
	}

	[[nodiscard]] MarmotCompiler::Optional<std::string_view> SuggestCommand(std::string_view input)
	{
		static constexpr std::string_view commands[] = { "check", "build", "fmt", "help" };
		MarmotCompiler::Optional<std::string_view> best_match = MarmotCompiler::None;
		int best_distance = 1000;
		for (const std::string_view command : commands)
		{
			const int distance = EditDistance(input, command);
			if (distance < best_distance)
			{
				best_distance = distance;
				best_match = command;
			}
		}

		if (best_match.HasValue() && best_distance <= 3)
		{
			return best_match;
		}

		return MarmotCompiler::None;
	}

	// A plan names its own entry, so it replaces the source-file argument.
	[[nodiscard]] MarmotCompiler::Expected<void, std::string> ApplyPlan(Invocation& invocation, const MarmotCompiler::Optional<std::filesystem::path>& plan_file, std::string_view command)
	{
		if (!plan_file.HasValue())
		{
			if (!invocation.m_show_help && invocation.m_source_file.empty())
			{
				return MarmotCompiler::Failure(std::format("Missing source file for {}.", command));
			}
			return {};
		}

		MarmotBuildPlan::PlanResult plan = MarmotBuildPlan::ReadFile(plan_file.Value());
		if (!plan.HasValue())
		{
			return MarmotCompiler::Failure(std::format("Invalid build plan: {}", plan.Error()));
		}

		// A plan without an entry gives only the inputs, and the file given
		// with it is the entry (the marmot tool compiles tests this way). A plan
		// with an entry is the whole compile.
		if (plan->m_entry.HasValue() && !invocation.m_source_file.empty())
		{
			return MarmotCompiler::Failure(std::format("Pass either a source file or a plan with an entry to {}, not both.", command));
		}

		if (!plan->m_entry.HasValue() && invocation.m_source_file.empty())
		{
			return MarmotCompiler::Failure(std::format("Invalid build plan: {}: {} needs an \"entry\", or a source file", plan_file->string(), command));
		}

		if (plan->m_entry.HasValue())
		{
			invocation.m_source_file = plan->m_entry.Value();
		}
		invocation.m_plan_inputs = std::move(plan->m_inputs);
		return {};
	}

	[[nodiscard]] std::string UnknownCommand(std::string_view command)
	{
		if (command == "run" || command == "test" || command.ends_with(".mmt") || command.ends_with(".mmc"))
		{
			return std::format(
				"marmotc compiles and never runs a program. Run one with `marmot {} <file>`, or build it with `marmotc build` and run the .mmc with marmotvm.",
				command == "test" ? "test" : "run");
		}
		const MarmotCompiler::Optional<std::string_view> suggestion = SuggestCommand(command);
		return suggestion ? std::format("Unknown command: {}. Did you mean `{}`?", command, *suggestion) : std::format("Unknown command: {}", command);
	}

	[[nodiscard]] ParseResult ParseInvocation(int argc, char* argv[])
	{
		Invocation invocation;
		std::vector<std::string> formats;
		std::string plan;
		std::string output;
		std::string deps;
		std::vector<std::string> jobs;
		std::string help_topic;
		std::vector<std::string> sources;
		std::vector<std::string> format_paths;
		bool version = false;
		bool overview_help = false;

		CLI::App app;
		app.set_help_flag();
		app.require_subcommand(0, 1);
		app.add_flag("-h,--help", overview_help);
		app.add_flag("--version", version);
		std::vector<std::string> global_formats;
		app.add_option("--format", global_formats)->expected(1)->take_all();

		CLI::App* check = app.add_subcommand("check");
		CLI::App* analyze = app.add_subcommand("analyze");
		CLI::App* build = app.add_subcommand("build");
		CLI::App* fmt = app.add_subcommand("fmt");
		CLI::App* help = app.add_subcommand("help");
		help->add_option("topic", help_topic);
		for (CLI::App* command : { check, analyze, build, fmt })
		{
			command->add_flag("-h,--help", invocation.m_show_help);
			command->add_option("--format", formats)->expected(1)->take_all();
		}
		for (CLI::App* command : { check, build })
		{
			command->add_option("source", sources);
			command->add_option("--plan", plan);
			command->add_option("--jobs", jobs)->expected(1)->take_all();
			command->add_flag("--timings", invocation.m_timings);
			command->add_flag("--optimizer-stats", invocation.m_optimizer_stats);
			command->add_flag("--emit-ir", invocation.m_emit_ir);
			command->add_flag("--emit-ast", invocation.m_emit_ast);
		}
		CLI::Option* output_option = build->add_option("-o,--output", output)->take_last();
		CLI::Option* deps_option = build->add_option("--deps", deps)->take_last();
		build->add_flag("--embed-sources", invocation.m_embed_sources);
		build->add_flag("--quiet", invocation.m_quiet);
		fmt->add_option("paths", format_paths);
		fmt->add_flag("-w,--write", invocation.m_fmt_write);
		fmt->add_flag("--check", invocation.m_fmt_check);

		try
		{
			app.parse(argc, argv);
		}
		catch (const CLI::ExtrasError& error)
		{
			const std::vector<std::string> extras = app.remaining(true);
			if (extras.empty())
			{
				return MarmotCompiler::Failure(error.what());
			}
			const std::string& extra = extras.front();
			return MarmotCompiler::Failure(extra.starts_with('-') ? "Unknown option: " + extra : UnknownCommand(extra));
		}
		catch (const CLI::ArgumentMismatch& error)
		{
			const std::string_view message = error.what();
			for (std::string_view option : { "--jobs", "--plan", "--format", "--output", "--deps" })
			{
				if (message.starts_with(option) && message.find("missing") != std::string_view::npos)
				{
					return MarmotCompiler::Failure(std::format("Missing value for {}.", option));
				}
			}
			if (message.starts_with("--plan"))
			{
				return MarmotCompiler::Failure("Only one --plan is allowed.");
			}
			return MarmotCompiler::Failure(error.what());
		}
		catch (const CLI::ParseError& error)
		{
			return MarmotCompiler::Failure(error.what());
		}

		for (const std::vector<std::string>* values : { &global_formats, &formats })
		{
			for (const std::string& format : *values)
			{
				if (format != "json")
				{
					return MarmotCompiler::Failure("Unknown format: " + format);
				}
			}
		}
		invocation.m_format = global_formats.empty() && formats.empty() ? OutputFormat::Text : OutputFormat::Json;
		if (overview_help || version)
		{
			invocation.m_show_help = overview_help;
			invocation.m_kind = version ? CommandKind::Version : CommandKind::Overview;
			return invocation;
		}

		if (*help)
		{
			invocation.m_show_help = true;
			const std::vector<CommandSpec>& commands = CommandTable();
			const auto topic = std::ranges::find(commands, help_topic, &CommandSpec::m_name);
			if (!help_topic.empty() && topic == commands.end())
			{
				return MarmotCompiler::Failure("Unknown help topic: " + help_topic);
			}
			if (topic != commands.end())
			{
				invocation.m_kind = topic->m_kind;
			}
		}
		else if (*fmt)
		{
			invocation.m_kind = CommandKind::Fmt;
			invocation.m_fmt_paths.assign(format_paths.begin(), format_paths.end());
			if (!invocation.m_show_help && invocation.m_fmt_paths.empty())
			{
				return MarmotCompiler::Failure("Missing file or directory for fmt.");
			}
		}
		else if (*analyze)
		{
			invocation.m_kind = CommandKind::Analyze;
		}
		else if (*check || *build)
		{
			const bool is_build = static_cast<bool>(*build);
			invocation.m_kind = is_build ? CommandKind::Build : CommandKind::Check;
			const std::string_view command = is_build ? "build" : "check";
			if (sources.size() > 1u)
			{
				return MarmotCompiler::Failure(std::format("Only one source file is allowed for {}.", command));
			}
			if (!sources.empty())
			{
				invocation.m_source_file = sources.front();
			}
			if (output_option->count() != 0u)
			{
				invocation.m_output_path = output;
			}
			if (deps_option->count() != 0u)
			{
				invocation.m_deps_path = deps;
			}

			const CLI::App* compile = is_build ? build : check;
			for (const std::string& value : jobs)
			{
				size_t count = 0;
				const std::from_chars_result number = std::from_chars(value.data(), value.data() + value.size(), count);
				if (number.ec != std::errc{} || number.ptr != value.data() + value.size() || count == 0u)
				{
					return MarmotCompiler::Failure("--jobs requires a positive integer.");
				}
				invocation.m_jobs = count;
			}
#if !MARMOT_ENABLE_OPTIMIZER_STATS
			if (invocation.m_optimizer_stats)
			{
				return MarmotCompiler::Failure("--optimizer-stats requires a Debug or Dev build.");
			}
#endif
			const MarmotCompiler::Optional<std::filesystem::path> plan_file = compile->count("--plan") != 0u
				? MarmotCompiler::Optional<std::filesystem::path>(plan)
				: MarmotCompiler::None;
			if (const MarmotCompiler::Expected<void, std::string> applied = ApplyPlan(invocation, plan_file, command); !applied)
			{
				return MarmotCompiler::Failure(applied.Error());
			}
		}
		else if (!global_formats.empty())
		{
			return MarmotCompiler::Failure("Missing command after global flags.");
		}

		if (invocation.m_emit_ir && invocation.m_format == OutputFormat::Json)
		{
			return MarmotCompiler::Failure("--emit-ir prints text, so it cannot be used with --format json.");
		}
		if (invocation.m_emit_ast && invocation.m_format == OutputFormat::Json)
		{
			return MarmotCompiler::Failure("--emit-ast prints text, so it cannot be used with --format json.");
		}
		return invocation;
	}

	MarmotJson::Document SourceSpanJson(const SourceSpan& span)
	{
		MarmotJson::Document result;
		result.AddStringField("file", span.m_file);
		result.AddNumberField("line", span.m_start.m_line);
		result.AddNumberField("column", span.m_start.m_column);
		result.AddNumberField("endLine", span.m_end.m_line);
		result.AddNumberField("endColumn", span.m_end.m_column);
		return result;
	}

	[[nodiscard]] std::string CommandJson(
		std::string_view command,
		bool success,
		const MarmotResult::CompilerReport& report,
		MarmotCompiler::Optional<int> exit_code = MarmotCompiler::None,
		std::string_view stdout_text = {},
		std::string_view stderr_text = {},
		MarmotCompiler::Optional<std::string_view> artifact_json = MarmotCompiler::None,
		const EditorSymbols* symbols = nullptr)
	{
		MarmotJson::Document payload;
		payload.AddNumberField("version", 1);
		payload.AddStringField("source", "marmot");
		payload.AddStringField("command", command);
		payload.AddBoolField("success", success);
		payload.AddNumberField("exitCode", exit_code);
		payload.AddStringField("stdout", stdout_text);
		payload.AddStringField("stderr", stderr_text);
		payload.AddRawField("report", report.MachineReadableJson());
		if (artifact_json.HasValue())
		{
			payload.AddRawField("artifact", *artifact_json);
		}
		if (symbols != nullptr)
		{
			MarmotJson::Document definitions = MarmotJson::Document::Array();
			for (const DefinitionLink& link : symbols->m_definitions)
			{
				MarmotJson::Document item;
				item.AddField("reference", SourceSpanJson(link.m_reference));
				MarmotJson::Document declaration = SourceSpanJson(link.m_declaration.m_span);
				declaration.AddNumberField("documentationLine", link.m_declaration.m_documentation_line);
				item.AddField("declaration", declaration);
				definitions.Append(item);
			}
			payload.AddField("definitions", definitions);

			MarmotJson::Document types = MarmotJson::Document::Array();
			for (const SymbolType& symbol : symbols->m_types)
			{
				MarmotJson::Document item;
				item.AddField("span", SourceSpanJson(symbol.m_span));
				item.AddStringField("type", symbol.m_type);
				item.AddNumberField("documentationLine", symbol.m_documentation_line);
				types.Append(item);
			}
			payload.AddField("types", types);
		}

		return payload.Write();
	}

	int HandleOverview(const Invocation&)
	{
		std::print("{}", GeneralHelp());
		return EXIT_SUCCESS;
	}

	int HandleVersion(const Invocation& invocation)
	{
		if (invocation.m_format == OutputFormat::Json)
		{
			MarmotJson::Document payload;
			payload.AddNumberField("version", 1);
			payload.AddStringField("source", "marmot");
			payload.AddStringField("command", "version");
			payload.AddBoolField("success", true);
			payload.AddStringField("marmotVersion", MARMOT_VERSION_STRING);

			std::print("{}", payload.Write());
		}
		else
		{
			std::print("marmotc {}\n", MARMOT_VERSION_STRING);
		}
		return EXIT_SUCCESS;
	}

	// Compiles the invocation's source file from its plan's inputs, or from the
	// inputs the CLI discovers when there is no plan.
	[[nodiscard]] MarmotDriver::CompileFileWithReportResult CompileInvocation(const Invocation& invocation)
	{
		CompilationInputs inputs = invocation.m_plan_inputs.ValueOr(MarmotDriver::EnvironmentCompilationInputs());
		return MarmotDriver::CompileFileWithReport(invocation.m_source_file, std::move(inputs)
			.WithEmitMarmotIR(invocation.m_emit_ir)
			.WithEmitAst(invocation.m_emit_ast)
			.WithJobs(invocation.m_jobs)
			.WithTimings(invocation.m_timings));
	}

	// Each module's syntax tree, then each module's MarmotIR, as asked.
	void PrintDumps(const MarmotResult::CompiledProgram& compiled_program)
	{
		for (const std::string& module_ast : compiled_program.m_ast)
		{
			std::print("{}\n", module_ast);
		}
		for (const std::string& module_ir : compiled_program.m_marmot_ir)
		{
			std::print("{}\n", module_ir);
		}
	}

	int HandleCheck(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("check"));
			return EXIT_SUCCESS;
		}

		const CompilerDiagnostics::ScopedStatistics statistics(invocation.m_optimizer_stats);
		const MarmotDriver::CompileFileWithReportResult compile_result = CompileInvocation(invocation);
		if (!compile_result.HasValue())
		{
			const MarmotResult::CompilerReport& report = compile_result.Error().m_report;
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("check", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print(stderr, "{}", compile_result.Error().Rendered());
			}
			return EXIT_FAILURE;
		}

		PrintDumps(*compile_result);
		const MarmotResult::CompilerReport& report = compile_result->Report();
		if (invocation.m_format == OutputFormat::Json)
		{
			std::print("{}", CommandJson("check", true, report, EXIT_SUCCESS));
		}
		else
		{
			std::print(stderr, "{}", report.RenderedWarnings());
		}

		return EXIT_SUCCESS;
	}

	[[nodiscard]] MarmotBuildPlan::PlanResult ReadAnalysisRequest()
	{
		std::ostringstream input;
		input << std::cin.rdbuf();
		const MarmotCompiler::Expected<CompilerJson::JsonValue, std::string> request = CompilerJson::Parse(input.str());
		if (!request)
		{
			return MarmotCompiler::Failure(request.Error());
		}

		const CompilerJson::JsonValue* plan_value = request->Find("plan");
		const CompilerJson::JsonValue* sources = request->Find("sources");
		if (plan_value == nullptr || sources == nullptr || !sources->IsObject())
		{
			return MarmotCompiler::Failure("Expected an analysis request with a plan and a sources object.");
		}

		MarmotBuildPlan::PlanResult plan = MarmotBuildPlan::Parse(*plan_value, std::filesystem::current_path());
		if (!plan)
		{
			return MarmotCompiler::Failure(plan.Error());
		}
		if (!plan->m_entry)
		{
			return MarmotCompiler::Failure("The analysis plan must name an entry.");
		}

		CompilationInputs::SourceOverlays overlays;
		for (const auto& [file_path, text] : sources->AsObject())
		{
			if (!std::filesystem::path(file_path).is_absolute() || !text.IsString())
			{
				return MarmotCompiler::Failure("Analysis sources must map absolute file paths to strings.");
			}
			overlays.emplace(file_path, text.AsString());
		}
		plan->m_inputs = std::move(plan->m_inputs).WithSourceOverlays(std::move(overlays));
		return plan;
	}

	int HandleAnalyze(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("analyze"));
			return EXIT_SUCCESS;
		}

		MarmotBuildPlan::PlanResult plan = ReadAnalysisRequest();
		if (!plan)
		{
			PrintCliError(plan.Error());
			return EXIT_FAILURE;
		}

		const EditorAnalysis analysis = MarmotDriver::AnalyzeFile(*plan->m_entry, std::move(plan->m_inputs));
		const bool success = analysis.m_report.m_errors.Empty();
		const int exit_code = success ? EXIT_SUCCESS : EXIT_FAILURE;
		std::print("{}", CommandJson("analyze", success, analysis.m_report, exit_code, {}, {}, MarmotCompiler::None, &analysis.m_symbols));
		return exit_code;
	}

	int HandleBuild(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("build"));
			return EXIT_SUCCESS;
		}

		const CompilerDiagnostics::ScopedStatistics statistics(invocation.m_optimizer_stats);
		const MarmotDriver::CompileFileWithReportResult compile_result = CompileInvocation(invocation);
		if (!compile_result.HasValue())
		{
			const MarmotResult::CompilerReport& report = compile_result.Error().m_report;
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("build", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print(stderr, "{}", compile_result.Error().Rendered());
			}
			return EXIT_FAILURE;
		}

		const MarmotResult::CompiledProgram& compiled_program = *compile_result;
		PrintDumps(compiled_program);
		const MarmotExecutable& executable = compiled_program.m_executable;

		std::filesystem::path artifact_path = invocation.m_output_path.ValueOr(invocation.m_source_file);
		if (!invocation.m_output_path.HasValue())
		{
			artifact_path.replace_extension(".mmc");
		}

		MarmotCompiler::Expected<void, std::string> write_result = {};
		std::error_code directory_error;
		if (artifact_path.has_parent_path())
		{
			std::filesystem::create_directories(artifact_path.parent_path(), directory_error);
		}
		if (directory_error)
		{
			write_result = MarmotCompiler::Failure(std::format("Could not create {}: {}", artifact_path.parent_path().string(), directory_error.message()));
		}
		else
		{
			write_result = MarmotBinaryArtifact::WriteExecutableToFile(executable, artifact_path, invocation.m_embed_sources);
		}

		if (write_result.HasValue() && invocation.m_deps_path.HasValue())
		{
			std::string listing;
			for (const std::string& source_file : compiled_program.m_source_files)
			{
				listing += source_file;
				listing.push_back('\n');
			}

			std::ofstream deps(invocation.m_deps_path.Value(), std::ios::binary | std::ios::trunc);
			deps.write(listing.data(), static_cast<std::streamsize>(listing.size()));
			if (!deps)
			{
				write_result = MarmotCompiler::Failure(std::format("Could not write {}", invocation.m_deps_path->string()));
			}
		}

		if (!write_result.HasValue())
		{
			MarmotResult::CompilerReport report = compiled_program.Report();
			report.m_errors.Append(MarmotResult::CompilerDiagnostics(
				CompilerError::Simple(CompilerStage::Compiler, write_result.Error())));
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("build", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print(stderr, "{}", report.Rendered());
			}
			return EXIT_FAILURE;
		}

		if (invocation.m_format == OutputFormat::Json)
		{
			MarmotJson::Document artifact_json;
			artifact_json.AddStringField("path", artifact_path.generic_string());
			artifact_json.AddStringField("entryFile", invocation.m_source_file.generic_string());
			artifact_json.AddNumberField("procedureCount", executable.GetProcedureCount());
			artifact_json.AddNumberField("globalCount", executable.GetGlobalVariableCount());
			artifact_json.AddNumberField("stringCount", static_cast<int>(executable.GetStringPool().size()));
			MarmotJson::Document native_libraries = MarmotJson::Document::Array();
			for (const NativeLibraryImport& library : executable.GetNativeLibraries())
			{
				native_libraries.AppendString(library.m_name);
			}
			artifact_json.AddField("nativeLibraries", native_libraries);
			std::print("{}", CommandJson("build", true, compiled_program.Report(), EXIT_SUCCESS, {}, {}, artifact_json.Write()));
			return EXIT_SUCCESS;
		}

		std::print(stderr, "{}", compiled_program.Report().RenderedWarnings());
		if (MarmotDriver::ShouldEmitMachineReadableWarnings())
		{
			std::print(stderr, "{}", compiled_program.Report().MachineReadableWarnings());
		}
		if (!invocation.m_quiet)
		{
			std::print
			(
				"Built {} -> {} (procedures={}, globals={}, strings={})\n",
				invocation.m_source_file.string(),
				artifact_path.string(),
				executable.GetProcedureCount(),
				executable.GetGlobalVariableCount(),
				executable.GetStringPool().size()
			);
		}
		return EXIT_SUCCESS;
	}

	int HandleFmt(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("fmt"));
			return EXIT_SUCCESS;
		}

		std::error_code error_code;
		for (const std::filesystem::path& path : invocation.m_fmt_paths)
		{
			if (!std::filesystem::is_directory(path, error_code) && !std::filesystem::is_regular_file(path, error_code))
			{
				PrintCliError(std::format("Could not find target for fmt: {}", path.string()));
				return EXIT_FAILURE;
			}
		}

		// Only a single file can be printed; anything more is checked or written.
		const std::filesystem::path& first_path = invocation.m_fmt_paths.front();
		const bool prints_one_file = invocation.m_fmt_paths.size() == 1u && std::filesystem::is_regular_file(first_path, error_code);
		if (!prints_one_file && !invocation.m_fmt_write && !invocation.m_fmt_check)
		{
			PrintCliError("Formatting a directory or several files requires --write or --check.");
			return EXIT_FAILURE;
		}

		if (prints_one_file && !invocation.m_fmt_write && !invocation.m_fmt_check)
		{
			const MarmotCompiler::Expected<std::string, std::string> read_result = [&]() -> MarmotCompiler::Expected<std::string, std::string>
			{
				std::ifstream input(first_path, std::ios::binary);
				if (!input.is_open())
				{
					return MarmotCompiler::Failure(std::format("Could not open file: {}", first_path.string()));
				}

				std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
				return text;
			}();
			if (!read_result.HasValue())
			{
				PrintCliError(read_result.Error());
				return EXIT_FAILURE;
			}

			const MarmotResult::Result<std::string> format_result = MarmotFormatter::FormatSource(read_result.Value(), first_path.string());
			if (!format_result.HasValue())
			{
				PrintCliError(format_result.Error().Rendered());
				return EXIT_FAILURE;
			}

			if (invocation.m_format == OutputFormat::Json)
			{
				MarmotJson::Document payload;
				payload.AddNumberField("version", 1);
				payload.AddStringField("source", "marmot");
				payload.AddStringField("command", "fmt");
				payload.AddBoolField("success", true);
				payload.AddStringField("target", first_path.generic_string());
				payload.AddStringField("formattedText", format_result.Value());

				std::print("{}", payload.Write());
			}
			else
			{
				std::print("{}", format_result.Value());
			}
			return EXIT_SUCCESS;
		}

		MarmotFormatter::RunResult result;
		for (const std::filesystem::path& path : invocation.m_fmt_paths)
		{
			MarmotFormatter::RunResult path_result = MarmotFormatter::FormatPath(
				path,
				MarmotFormatter::Options{ invocation.m_fmt_write, invocation.m_fmt_check });
			std::ranges::move(path_result.m_files, std::back_inserter(result.m_files));
		}

		if (invocation.m_format == OutputFormat::Json)
		{
			MarmotJson::Document files_json = MarmotJson::Document::Array();
			for (const MarmotFormatter::FileResult& file : result.m_files)
			{
				MarmotJson::Document object;
				object.AddStringField("path", file.m_path.generic_string());
				object.AddBoolField("changed", file.m_changed);
				object.AddBoolField("written", file.m_written);
				object.AddStringField("error", file.m_error.HasValue() ? MarmotCompiler::Optional<std::string_view>(*file.m_error) : MarmotCompiler::None);
				files_json.Append(object);
			}

			MarmotJson::Document payload;
			payload.AddNumberField("version", 1);
			payload.AddStringField("source", "marmot");
			payload.AddStringField("command", "fmt");
			payload.AddBoolField("success", !result.HasErrors() && (!invocation.m_fmt_check || !result.HasChanges()));
			payload.AddNumberField("changedCount", result.ChangedCount());
			payload.AddNumberField("errorCount", result.ErrorCount());
			payload.AddField("files", files_json);

			std::print("{}", payload.Write());
		}
		else
		{
			for (const MarmotFormatter::FileResult& file : result.m_files)
			{
				if (file.m_error.HasValue())
				{
					PrintCliError(std::format("{}: {}", file.m_path.string(), *file.m_error));
				}
			}

			if (invocation.m_fmt_check)
			{
				std::print(
					"Checked {} file(s); {} would change.\n",
					result.m_files.size(),
					result.ChangedCount());
			}
			else
			{
				std::print(
					"Formatted {} file(s); {} changed.\n",
					result.m_files.size(),
					result.ChangedCount());
			}
		}

		if (result.HasErrors())
		{
			return EXIT_FAILURE;
		}

		if (invocation.m_fmt_check && result.HasChanges())
		{
			return EXIT_FAILURE;
		}

		return EXIT_SUCCESS;
	}

	const std::vector<CommandSpec>& CommandTable()
	{
		static const std::vector<CommandSpec> table
		{
			{ "check", "Type-check a source file without executing it", CommandKind::Check, &HandleCheck },
			{ "analyze", "Analyze editor buffers and report diagnostics as JSON", CommandKind::Analyze, &HandleAnalyze },
			{ "build", "Compile a source file and report bytecode stats", CommandKind::Build, &HandleBuild },
			{ "fmt", "Format one file or a directory of .mmt files", CommandKind::Fmt, &HandleFmt }
		};
		return table;
	}

	[[nodiscard]] CommandHandler FindHandler(CommandKind kind)
	{
		if (kind == CommandKind::Overview)
		{
			return &HandleOverview;
		}
		if (kind == CommandKind::Version)
		{
			return &HandleVersion;
		}

		return std::ranges::find(CommandTable(), kind, &CommandSpec::m_kind)->m_handler;
	}

	[[nodiscard]] std::string_view CommandName(CommandKind kind)
	{
		for (const CommandSpec& spec : CommandTable())
		{
			if (spec.m_kind == kind)
			{
				return spec.m_name;
			}
		}

		return {};
	}
}

namespace MarmotCLI
{
	int Run(int argc, char* argv[])
	{
		MarmotTerminal::SetColorEnabled(MarmotTerminal::ShouldUseColor(stderr));
		const ParseResult parse_result = ParseInvocation(argc, argv);
		if (!parse_result.HasValue())
		{
			std::string message = parse_result.Error();
			if (argc >= 2)
			{
				const std::string_view head = argv[1];
				const MarmotCompiler::Optional<std::string_view> suggestion = SuggestCommand(head);
				if (suggestion.HasValue())
				{
					message += std::format(" Did you mean '{}'?", *suggestion);
				}
			}

			PrintCliError(message);
			std::print("{}", GeneralHelp());
			return EXIT_FAILURE;
		}

		const Invocation invocation = parse_result.Value();
		if (invocation.m_show_help)
		{
			const std::string_view command_name = CommandName(invocation.m_kind);
			if (command_name.empty())
			{
				std::print("{}", GeneralHelp());
			}
			else
			{
				std::print("{}", CommandHelp(command_name));
			}
			return EXIT_SUCCESS;
		}

		const CommandHandler handler = FindHandler(invocation.m_kind);
		return handler(invocation);
	}
}
