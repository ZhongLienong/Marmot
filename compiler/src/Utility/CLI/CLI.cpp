#include "Utility/CLI/CLI.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Common/Json/Json.h"
#include "Common/Printer/Printer.h"
#include "Utility/BuildPlan/BuildPlan.h"
#include "Utility/Driver/MidoriDriver.h"
#include "Utility/Formatter/Formatter.h"

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
		std::optional<CompilationInputs> m_plan_inputs = std::nullopt;
		std::vector<std::filesystem::path> m_fmt_paths;
		bool m_fmt_write = false;
		bool m_fmt_check = false;
		bool m_embed_sources = false;
		// build: where the .mmc goes (-o), where to list the files it was built
		// from (--deps), and whether to leave out the summary.
		std::optional<std::filesystem::path> m_output_path = std::nullopt;
		std::optional<std::filesystem::path> m_deps_path = std::nullopt;
		bool m_quiet = false;
	};

	using ParseResult = std::expected<Invocation, std::string>;
	using CommandHandler = int(*)(const Invocation&);

	struct CommandSpec
	{
		std::string_view m_name;
		std::string_view m_summary;
		CommandKind m_kind;
		CommandHandler m_handler;
	};

	[[nodiscard]] MidoriResult::CompilerReport WrapDriverErrorAsReport(const MidoriDriver::DriverError& error)
	{
		if (error.m_report.has_value())
		{
			return *error.m_report;
		}

		return MidoriResult::CompilerReport(MidoriResult::CompilerDiagnostics(
			CompilerError::Simple(CompilerStage::Compiler, error.m_message)));
	}

	[[nodiscard]] bool ShouldEmitMachineReadableWarnings()
	{
		const char* warning_format = std::getenv("MARMOT_TEST_WARNING_FORMAT");
		return warning_format != nullptr && std::string_view(warning_format) == "machine";
	}

	void PrintCliError(std::string_view message)
	{
		Printer::Print<Printer::Color::RED>(std::string(message));
		Printer::Print<Printer::Color::RED>("\n");
	}

	[[nodiscard]] std::string CommandHelp(std::string_view command_name)
	{
		if (command_name == "check")
		{
			return
				"Usage: marmotc check (<file> | --plan <plan.json>) [--format json]\n"
				"Type-check a Marmot source file without executing it.\n"
				"With --plan, check the plan's entry from exactly the plan's inputs.\n\n"
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
				"Examples:\n"
				"  marmotc build src/Main.mmt\n"
				"  marmotc build src/Main.mmt -o target/Main.mmc\n"
				"  marmotc build src/Main.mmt --embed-sources\n"
				"  marmotc build src/Main.mmt --format json\n"
				"  marmotc build --plan build/plan.json\n";
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
			"  marmotvm target/Main.mmc\n\n"
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

	[[nodiscard]] std::optional<std::string_view> SuggestCommand(std::string_view input)
	{
		static constexpr std::string_view commands[] = { "check", "build", "fmt", "help" };
		std::optional<std::string_view> best_match = std::nullopt;
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

		if (best_match.has_value() && best_distance <= 3)
		{
			return best_match;
		}

		return std::nullopt;
	}

	[[nodiscard]] bool ParseFormatValue(const std::vector<std::string_view>& args, size_t& index, OutputFormat& format, std::string& error)
	{
		if (index + 1u >= args.size())
		{
			error = "Missing value for --format.";
			return false;
		}

		const std::string_view format_value = args[index + 1u];
		if (format_value != "json")
		{
			error = std::format("Unknown format: {}", format_value);
			return false;
		}

		format = OutputFormat::Json;
		index += 1u;
		return true;
	}

	// Reads the value of --plan at args[index + 1].
	[[nodiscard]] bool ParsePlanValue(const std::vector<std::string_view>& args, size_t& index, std::optional<std::filesystem::path>& plan_file, std::string& error)
	{
		if (index + 1u >= args.size())
		{
			error = "Missing value for --plan.";
			return false;
		}

		if (plan_file.has_value())
		{
			error = "Only one --plan is allowed.";
			return false;
		}

		plan_file = std::filesystem::path(args[index + 1u]);
		index += 1u;
		return true;
	}

	// A plan names its own entry, so it replaces the source-file argument.
	[[nodiscard]] std::expected<void, std::string> ApplyPlan(Invocation& invocation, const std::optional<std::filesystem::path>& plan_file, std::string_view command)
	{
		if (!plan_file.has_value())
		{
			if (!invocation.m_show_help && invocation.m_source_file.empty())
			{
				return std::unexpected(std::format("Missing source file for {}.", command));
			}
			return {};
		}

		std::expected<BuildPlan, std::string> plan = MidoriBuildPlan::ReadFile(plan_file.value());
		if (!plan.has_value())
		{
			return std::unexpected(std::format("Invalid build plan: {}", plan.error()));
		}

		// A plan without an entry gives only the inputs, and the file given
		// with it is the entry (the marmot tool compiles tests this way). A plan
		// with an entry is the whole compile.
		if (plan->m_entry.has_value() && !invocation.m_source_file.empty())
		{
			return std::unexpected(std::format("Pass either a source file or a plan with an entry to {}, not both.", command));
		}

		if (!plan->m_entry.has_value() && invocation.m_source_file.empty())
		{
			return std::unexpected(std::format("Invalid build plan: {}: {} needs an \"entry\", or a source file", plan_file->string(), command));
		}

		if (plan->m_entry.has_value())
		{
			invocation.m_source_file = plan->m_entry.value();
		}
		invocation.m_plan_inputs = std::move(plan->m_inputs);
		return {};
	}

	[[nodiscard]] ParseResult ParseCompileLike(CommandKind kind, const std::vector<std::string_view>& args)
	{
		Invocation invocation;
		invocation.m_kind = kind;
		std::optional<std::filesystem::path> plan_file = std::nullopt;

		for (size_t index = 0u; index < args.size(); index += 1u)
		{
			const std::string_view arg = args[index];
			if (arg == "-h" || arg == "--help")
			{
				invocation.m_show_help = true;
				continue;
			}

			if (arg == "--plan")
			{
				std::string error;
				if (!ParsePlanValue(args, index, plan_file, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			if (arg == "--format")
			{
				std::string error;
				if (!ParseFormatValue(args, index, invocation.m_format, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			if (!arg.empty() && arg.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", arg));
			}

			if (!invocation.m_source_file.empty())
			{
				return std::unexpected(std::format("Only one source file is allowed for {}.", kind == CommandKind::Check ? "check" : "build"));
			}

			invocation.m_source_file = std::filesystem::path(arg);
		}

		const std::expected<void, std::string> plan_result = ApplyPlan(invocation, plan_file, kind == CommandKind::Check ? "check" : "build");
		if (!plan_result.has_value())
		{
			return std::unexpected(plan_result.error());
		}

		return invocation;
	}

	[[nodiscard]] ParseResult ParseBuild(const std::vector<std::string_view>& args)
	{
		Invocation invocation;
		invocation.m_kind = CommandKind::Build;
		std::optional<std::filesystem::path> plan_file = std::nullopt;

		for (size_t index = 0u; index < args.size(); index += 1u)
		{
			const std::string_view arg = args[index];
			if (arg == "-h" || arg == "--help")
			{
				invocation.m_show_help = true;
				continue;
			}

			if (arg == "--plan")
			{
				std::string error;
				if (!ParsePlanValue(args, index, plan_file, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			if (arg == "--embed-sources")
			{
				invocation.m_embed_sources = true;
				continue;
			}

			if (arg == "--quiet")
			{
				invocation.m_quiet = true;
				continue;
			}

			if (arg == "-o" || arg == "--output")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected(std::format("Missing value for {}.", arg));
				}
				index += 1u;
				invocation.m_output_path = std::filesystem::path(args[index]);
				continue;
			}

			if (arg == "--deps")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --deps.");
				}
				index += 1u;
				invocation.m_deps_path = std::filesystem::path(args[index]);
				continue;
			}

			if (arg == "--format")
			{
				std::string error;
				if (!ParseFormatValue(args, index, invocation.m_format, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			if (!arg.empty() && arg.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", arg));
			}

			if (!invocation.m_source_file.empty())
			{
				return std::unexpected("Only one source file is allowed for build.");
			}

			invocation.m_source_file = std::filesystem::path(arg);
		}

		const std::expected<void, std::string> plan_result = ApplyPlan(invocation, plan_file, "build");
		if (!plan_result.has_value())
		{
			return std::unexpected(plan_result.error());
		}

		return invocation;
	}

	[[nodiscard]] ParseResult ParseFmt(const std::vector<std::string_view>& args)
	{
		Invocation invocation;
		invocation.m_kind = CommandKind::Fmt;

		for (size_t index = 0u; index < args.size(); index += 1u)
		{
			const std::string_view arg = args[index];
			if (arg == "-h" || arg == "--help")
			{
				invocation.m_show_help = true;
				continue;
			}

			if (arg == "-w" || arg == "--write")
			{
				invocation.m_fmt_write = true;
				continue;
			}

			if (arg == "--check")
			{
				invocation.m_fmt_check = true;
				continue;
			}

			if (arg == "--format")
			{
				std::string error;
				if (!ParseFormatValue(args, index, invocation.m_format, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			if (!arg.empty() && arg.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", arg));
			}

			invocation.m_fmt_paths.emplace_back(arg);
		}

		if (!invocation.m_show_help && invocation.m_fmt_paths.empty())
		{
			return std::unexpected("Missing file or directory for fmt.");
		}

		return invocation;
	}

	[[nodiscard]] ParseResult ParseInvocation(int argc, char* argv[])
	{
		if (argc < 2)
		{
			return Invocation{ CommandKind::Overview };
		}

		std::vector<std::string_view> args;
		args.reserve(static_cast<size_t>(argc - 1));
		for (int index = 1; index < argc; index += 1)
		{
			args.emplace_back(argv[index]);
		}

		OutputFormat global_format = OutputFormat::Text;
		size_t offset = 0u;
		for (; offset < args.size(); offset += 1u)
		{
			if (args[offset] == "--format")
			{
				std::string error;
				if (!ParseFormatValue(args, offset, global_format, error))
				{
					return std::unexpected(error);
				}
				continue;
			}

			break;
		}

		if (offset >= args.size())
		{
			return std::unexpected("Missing command after global flags.");
		}

		const std::string_view head = args[offset];
		const std::vector<std::string_view> rest(args.begin() + static_cast<std::ptrdiff_t>(offset + 1u), args.end());

		if (head == "-h" || head == "--help")
		{
			return Invocation{ CommandKind::Overview, global_format, true };
		}

		if (head == "--version")
		{
			return Invocation{ CommandKind::Version, global_format };
		}

		if (head == "help")
		{
			if (rest.empty())
			{
				return Invocation{ CommandKind::Overview, global_format, true };
			}

			Invocation invocation;
			if (rest[0] == "check")
			{
				invocation.m_kind = CommandKind::Check;
			}
			else if (rest[0] == "build")
			{
				invocation.m_kind = CommandKind::Build;
			}
			else if (rest[0] == "fmt")
			{
				invocation.m_kind = CommandKind::Fmt;
			}
			else
			{
				return std::unexpected(std::format("Unknown help topic: {}", rest[0]));
			}

			invocation.m_show_help = true;
			invocation.m_format = global_format;
			return invocation;
		}

		ParseResult parsed;
		if (head == "check")
		{
			parsed = ParseCompileLike(CommandKind::Check, rest);
		}
		else if (head == "build")
		{
			parsed = ParseBuild(rest);
		}
		else if (head == "fmt")
		{
			parsed = ParseFmt(rest);
		}
		else
		{
			if (!head.empty() && head.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", head));
			}

			if (head == "run" || head == "test" || head.ends_with(".mmt") || head.ends_with(".mmc"))
			{
				return std::unexpected(std::format(
					"marmotc compiles and never runs a program. Run one with `marmot {} <file>`, or build it with `marmotc build` and run the .mmc with marmotvm.",
					head == "test" ? "test" : "run"));
			}

			const std::optional<std::string_view> suggestion = SuggestCommand(head);
			return std::unexpected(suggestion.has_value()
				? std::format("Unknown command: {}. Did you mean `{}`?", head, suggestion.value())
				: std::format("Unknown command: {}", head));
		}

		if (!parsed.has_value())
		{
			return parsed;
		}

		Invocation invocation = parsed.value();
		if (global_format == OutputFormat::Json)
		{
			invocation.m_format = global_format;
		}
		return invocation;
	}

	[[nodiscard]] std::string CommandJson(
		std::string_view command,
		bool success,
		const MidoriResult::CompilerReport& report,
		std::optional<int> exit_code = std::nullopt,
		std::string_view stdout_text = {},
		std::string_view stderr_text = {},
		std::optional<std::string_view> artifact_json = std::nullopt,
		std::optional<std::string_view> report_json = std::nullopt)
	{
		const std::string resolved_report_json = report_json.has_value() ? std::string(*report_json) : report.MachineReadableJson();
		std::string payload = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(payload, "version", 1, first_field);
		MidoriJson::AppendStringField(payload, "source", "marmot", first_field);
		MidoriJson::AppendStringField(payload, "command", command, first_field);
		MidoriJson::AppendBoolField(payload, "success", success, first_field);
		MidoriJson::AppendNumberField(payload, "exitCode", exit_code, first_field);
		MidoriJson::AppendStringField(payload, "stdout", stdout_text, first_field);
		MidoriJson::AppendStringField(payload, "stderr", stderr_text, first_field);
		MidoriJson::AppendRawField(payload, "report", resolved_report_json, first_field);
		if (artifact_json.has_value())
		{
			MidoriJson::AppendRawField(payload, "artifact", *artifact_json, first_field);
		}
		payload.push_back('}');
		return payload;
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
			std::string payload = "{";
			bool first_field = true;
			MidoriJson::AppendNumberField(payload, "version", 1, first_field);
			MidoriJson::AppendStringField(payload, "source", "marmot", first_field);
			MidoriJson::AppendStringField(payload, "command", "version", first_field);
			MidoriJson::AppendBoolField(payload, "success", true, first_field);
			MidoriJson::AppendStringField(payload, "marmotVersion", MidoriBuild::VersionString, first_field);
			payload.push_back('}');
			std::print("{}", payload);
		}
		else
		{
			std::print("marmotc {}\n", MidoriBuild::VersionString);
		}
		return EXIT_SUCCESS;
	}

	// Compiles the invocation's source file from its plan's inputs, or from the
	// inputs the CLI discovers when there is no plan.
	[[nodiscard]] MidoriDriver::CompileFileWithReportResult CompileInvocation(const Invocation& invocation)
	{
		if (invocation.m_plan_inputs.has_value())
		{
			return MidoriDriver::CompileFileWithReport(invocation.m_source_file, invocation.m_plan_inputs.value());
		}

		return MidoriDriver::CompileFileWithReport(invocation.m_source_file);
	}

	int HandleCheck(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("check"));
			return EXIT_SUCCESS;
		}

		const MidoriBuild::ScopedTestModeOverride suppress_internal_diagnostics(true);
		const MidoriDriver::CompileFileWithReportResult compile_result = CompileInvocation(invocation);
		if (!compile_result.has_value())
		{
			const MidoriResult::CompilerReport report = WrapDriverErrorAsReport(compile_result.error());
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("check", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print("{}", compile_result.error().Rendered());
			}
			return EXIT_FAILURE;
		}

		const MidoriResult::CompilerReport& report = compile_result->Report();
		if (invocation.m_format == OutputFormat::Json)
		{
			std::print("{}", CommandJson("check", true, report, EXIT_SUCCESS));
		}
		else
		{
			std::print("{}", report.RenderedWarnings());
		}

		return EXIT_SUCCESS;
	}

	int HandleBuild(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("build"));
			return EXIT_SUCCESS;
		}

		const MidoriBuild::ScopedTestModeOverride suppress_internal_diagnostics(true);
		const MidoriDriver::CompileFileWithReportResult compile_result = CompileInvocation(invocation);
		if (!compile_result.has_value())
		{
			const MidoriResult::CompilerReport report = WrapDriverErrorAsReport(compile_result.error());
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("build", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print("{}", compile_result.error().Rendered());
			}
			return EXIT_FAILURE;
		}

		const MidoriResult::CompiledProgram& compiled_program = *compile_result;
		const MidoriExecutable& executable = compiled_program.m_executable;

		std::filesystem::path artifact_path = invocation.m_output_path.value_or(invocation.m_source_file);
		if (!invocation.m_output_path.has_value())
		{
			artifact_path.replace_extension(".mmc");
		}

		std::expected<void, std::string> write_result = {};
		std::error_code directory_error;
		if (artifact_path.has_parent_path())
		{
			std::filesystem::create_directories(artifact_path.parent_path(), directory_error);
		}
		if (directory_error)
		{
			write_result = std::unexpected(std::format("Could not create {}: {}", artifact_path.parent_path().string(), directory_error.message()));
		}
		else
		{
			write_result = MidoriBinaryArtifact::WriteExecutableToFile(executable, artifact_path, invocation.m_embed_sources);
		}

		if (write_result.has_value() && invocation.m_deps_path.has_value())
		{
			std::string listing;
			for (const std::string& source_file : compiled_program.m_source_files)
			{
				listing += source_file;
				listing.push_back('\n');
			}

			std::ofstream deps(invocation.m_deps_path.value(), std::ios::binary | std::ios::trunc);
			deps.write(listing.data(), static_cast<std::streamsize>(listing.size()));
			if (!deps)
			{
				write_result = std::unexpected(std::format("Could not write {}", invocation.m_deps_path->string()));
			}
		}

		if (!write_result.has_value())
		{
			MidoriResult::CompilerReport report = compiled_program.Report();
			report.AppendErrors(MidoriResult::CompilerDiagnostics(
				CompilerError::Simple(CompilerStage::Compiler, write_result.error())));
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("build", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print("{}", report.Rendered());
			}
			return EXIT_FAILURE;
		}

		if (invocation.m_format == OutputFormat::Json)
		{
			std::string artifact_json = "{";
			bool first_field = true;
			MidoriJson::AppendStringField(artifact_json, "path", artifact_path.generic_string(), first_field);
			MidoriJson::AppendStringField(artifact_json, "entryFile", invocation.m_source_file.generic_string(), first_field);
			MidoriJson::AppendNumberField(artifact_json, "procedureCount", executable.GetProcedureCount(), first_field);
			MidoriJson::AppendNumberField(artifact_json, "globalCount", executable.GetGlobalVariableCount(), first_field);
			MidoriJson::AppendNumberField(artifact_json, "stringCount", static_cast<int>(executable.GetStringPool().size()), first_field);
			artifact_json.push_back('}');
			std::print("{}", CommandJson("build", true, compiled_program.Report(), EXIT_SUCCESS, {}, {}, artifact_json));
			return EXIT_SUCCESS;
		}

		std::print("{}", compiled_program.Report().RenderedWarnings());
		if (ShouldEmitMachineReadableWarnings())
		{
			std::print("{}", compiled_program.Report().MachineReadableWarnings());
		}
		if (!invocation.m_quiet)
		{
			std::print(
				"Built {} -> {} (procedures={}, globals={}, strings={})\n",
				invocation.m_source_file.string(),
				artifact_path.string(),
				executable.GetProcedureCount(),
				executable.GetGlobalVariableCount(),
				executable.GetStringPool().size());
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
			const std::expected<std::string, std::string> read_result = [&]() -> std::expected<std::string, std::string>
			{
				std::ifstream input(first_path, std::ios::binary);
				if (!input.is_open())
				{
					return std::unexpected(std::format("Could not open file: {}", first_path.string()));
				}

				std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
				return text;
			}();
			if (!read_result.has_value())
			{
				PrintCliError(read_result.error());
				return EXIT_FAILURE;
			}

			const std::expected<std::string, CompilerError> format_result = MidoriFormatter::FormatSource(read_result.value(), first_path.string());
			if (!format_result.has_value())
			{
				PrintCliError(format_result.error().Rendered());
				return EXIT_FAILURE;
			}

			if (invocation.m_format == OutputFormat::Json)
			{
				std::string payload = "{";
				bool first_field = true;
				MidoriJson::AppendNumberField(payload, "version", 1, first_field);
				MidoriJson::AppendStringField(payload, "source", "marmot", first_field);
				MidoriJson::AppendStringField(payload, "command", "fmt", first_field);
				MidoriJson::AppendBoolField(payload, "success", true, first_field);
				MidoriJson::AppendStringField(payload, "target", first_path.generic_string(), first_field);
				MidoriJson::AppendStringField(payload, "formattedText", format_result.value(), first_field);
				payload.push_back('}');
				std::print("{}", payload);
			}
			else
			{
				std::print("{}", format_result.value());
			}
			return EXIT_SUCCESS;
		}

		MidoriFormatter::RunResult result;
		for (const std::filesystem::path& path : invocation.m_fmt_paths)
		{
			MidoriFormatter::RunResult path_result = MidoriFormatter::FormatPath(
				path,
				MidoriFormatter::Options{ invocation.m_fmt_write, invocation.m_fmt_check });
			std::ranges::move(path_result.m_files, std::back_inserter(result.m_files));
		}

		if (invocation.m_format == OutputFormat::Json)
		{
			std::string files_json = "[";
			for (size_t index = 0u; index < result.m_files.size(); index += 1u)
			{
				if (index > 0u)
				{
					files_json.push_back(',');
				}

				const MidoriFormatter::FileResult& file = result.m_files[index];
				std::string object = "{";
				bool first_field = true;
				MidoriJson::AppendStringField(object, "path", file.m_path.generic_string(), first_field);
				MidoriJson::AppendBoolField(object, "changed", file.m_changed, first_field);
				MidoriJson::AppendBoolField(object, "written", file.m_written, first_field);
				MidoriJson::AppendStringField(
					object,
					"error",
					file.m_error.has_value() ? std::optional<std::string_view>(*file.m_error) : std::nullopt,
					first_field);
				object.push_back('}');
				files_json += object;
			}
			files_json.push_back(']');

			std::string payload = "{";
			bool first_field = true;
			MidoriJson::AppendNumberField(payload, "version", 1, first_field);
			MidoriJson::AppendStringField(payload, "source", "marmot", first_field);
			MidoriJson::AppendStringField(payload, "command", "fmt", first_field);
			MidoriJson::AppendBoolField(payload, "success", !result.HasErrors() && (!invocation.m_fmt_check || !result.HasChanges()), first_field);
			MidoriJson::AppendNumberField(payload, "changedCount", result.ChangedCount(), first_field);
			MidoriJson::AppendNumberField(payload, "errorCount", result.ErrorCount(), first_field);
			MidoriJson::AppendRawField(payload, "files", files_json, first_field);
			payload.push_back('}');
			std::print("{}", payload);
		}
		else
		{
			for (const MidoriFormatter::FileResult& file : result.m_files)
			{
				if (file.m_error.has_value())
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

	int HandleUnsupportedJson(const Invocation& invocation)
	{
		if (invocation.m_format == OutputFormat::Json)
		{
			PrintCliError("JSON output is not supported for this command.");
			return EXIT_FAILURE;
		}

		return EXIT_SUCCESS;
	}

	const std::vector<CommandSpec>& CommandTable()
	{
		static const std::vector<CommandSpec> table
		{
			{ "check", "Type-check a source file without executing it", CommandKind::Check, &HandleCheck },
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

		for (const CommandSpec& spec : CommandTable())
		{
			if (spec.m_kind == kind)
			{
				return spec.m_handler;
			}
		}

		return &HandleUnsupportedJson;
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

namespace MidoriCLI
{
	int Run(int argc, char* argv[])
	{
		const ParseResult parse_result = ParseInvocation(argc, argv);
		if (!parse_result.has_value())
		{
			std::string message = parse_result.error();
			if (argc >= 2)
			{
				const std::string_view head = argv[1];
				const std::optional<std::string_view> suggestion = SuggestCommand(head);
				if (suggestion.has_value())
				{
					message += std::format(" Did you mean '{}'?", *suggestion);
				}
			}

			PrintCliError(message);
			std::print("{}", GeneralHelp());
			return EXIT_FAILURE;
		}

		const Invocation invocation = parse_result.value();
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
