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
#include "Utility/OutputCapture/OutputCapture.h"
#include "Utility/TestRunner/TestRunner.h"

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
		Run,
		Check,
		Build,
		Fmt,
		Test,
		TestWorker
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
		// run: how the program's native libraries are found (--library-path and
		// the plan's).
		NativeLibraryOptions m_native_options;
		std::filesystem::path m_target_path;
		std::filesystem::path m_worker_result_directory;
		bool m_fmt_write = false;
		bool m_fmt_check = false;
		bool m_embed_sources = false;
		std::optional<std::string> m_test_filter = std::nullopt;
		std::optional<std::string> m_test_pattern = std::nullopt;
		std::optional<std::string> m_test_file = std::nullopt;
		// test: the test directory, the per-test timeout, and the plan whose
		// inputs every test compiles with.
		std::optional<std::filesystem::path> m_test_directory = std::nullopt;
		std::optional<int> m_test_timeout_ms = std::nullopt;
		std::optional<std::filesystem::path> m_plan_file = std::nullopt;
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

	struct BuildArtifactResult
	{
		std::filesystem::path m_path;
		std::string m_json;
	};

	[[nodiscard]] std::string SerializeStringArray(const std::vector<std::string>& values)
	{
		std::string serialized = "[";
		for (size_t index = 0u; index < values.size(); index += 1u)
		{
			if (index > 0u)
			{
				serialized.push_back(',');
			}

			serialized.push_back('\"');
			serialized += MidoriJson::EscapeString(values[index]);
			serialized.push_back('\"');
		}
		serialized.push_back(']');
		return serialized;
	}

	[[nodiscard]] std::string SerializeProcedureArtifact(const MidoriExecutable& executable, int proc_index)
	{
		const int instruction_count = executable.GetByteCodeSize(proc_index);
		std::string opcodes_json = "[";
		std::string lines_json = "[";
		for (int instruction_index = 0; instruction_index < instruction_count; instruction_index += 1)
		{
			if (instruction_index > 0)
			{
				opcodes_json.push_back(',');
				lines_json.push_back(',');
			}

			opcodes_json += std::to_string(static_cast<int>(executable.ReadByteCode(instruction_index, proc_index)));
			lines_json += std::to_string(executable.GetLine(instruction_index, proc_index));
		}
		opcodes_json.push_back(']');
		lines_json.push_back(']');

		std::string object = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(object, "index", proc_index, first_field);
		MidoriJson::AppendStringField(object, "name", executable.m_procedure_names[static_cast<size_t>(proc_index)], first_field);
		MidoriJson::AppendNumberField(object, "instructionCount", instruction_count, first_field);
		MidoriJson::AppendRawField(object, "opcodes", opcodes_json, first_field);
		MidoriJson::AppendRawField(object, "lines", lines_json, first_field);
		object.push_back('}');
		return object;
	}

	[[nodiscard]] std::expected<BuildArtifactResult, std::string> WriteBuildArtifact(
		const MidoriExecutable& executable,
		const std::filesystem::path& source_file)
	{
		std::filesystem::path artifact_path = source_file;
		artifact_path.replace_extension(".mmc.json");

		std::vector<std::string> globals;
		globals.reserve(static_cast<size_t>(executable.GetGlobalVariableCount()));
		for (int index = 0; index < executable.GetGlobalVariableCount(); index += 1)
		{
			globals.emplace_back(executable.GetGlobalVariable(index));
		}

		const std::vector<std::string> strings = executable.GetStringPool();

		int total_instruction_count = 0;
		std::string procedures_json = "[";
		for (int proc_index = 0; proc_index < executable.GetProcedureCount(); proc_index += 1)
		{
			if (proc_index > 0u)
			{
				procedures_json.push_back(',');
			}

			total_instruction_count += executable.GetByteCodeSize(proc_index);
			procedures_json += SerializeProcedureArtifact(executable, proc_index);
		}
		procedures_json.push_back(']');

		std::string artifact_json = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(artifact_json, "version", 1, first_field);
		MidoriJson::AppendStringField(artifact_json, "kind", "marmot-bytecode", first_field);
		MidoriJson::AppendStringField(artifact_json, "path", artifact_path.generic_string(), first_field);
		MidoriJson::AppendStringField(artifact_json, "entryFile", source_file.generic_string(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "procedureCount", executable.GetProcedureCount(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "globalCount", executable.GetGlobalVariableCount(), first_field);
		MidoriJson::AppendNumberField(artifact_json, "stringCount", static_cast<int>(strings.size()), first_field);
		MidoriJson::AppendNumberField(artifact_json, "instructionCount", total_instruction_count, first_field);
		MidoriJson::AppendRawField(artifact_json, "globals", SerializeStringArray(globals), first_field);
		MidoriJson::AppendRawField(artifact_json, "strings", SerializeStringArray(strings), first_field);
		MidoriJson::AppendRawField(artifact_json, "procedures", procedures_json, first_field);
		artifact_json.push_back('}');

		std::ofstream output(artifact_path, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			return std::unexpected(std::format("Could not open bytecode artifact for writing: {}", artifact_path.string()));
		}

		output.write(artifact_json.data(), static_cast<std::streamsize>(artifact_json.size()));
		if (!output)
		{
			return std::unexpected(std::format("Could not write bytecode artifact: {}", artifact_path.string()));
		}

		return BuildArtifactResult
		{
			.m_path = artifact_path,
			.m_json = artifact_json
		};
	}

	void PrintCliError(std::string_view message)
	{
		Printer::Print<Printer::Color::RED>(std::string(message));
		Printer::Print<Printer::Color::RED>("\n");
	}

	[[nodiscard]] std::string CommandHelp(std::string_view command_name)
	{
		if (command_name == "run")
		{
			return
				"Usage: marmotc run (<file> | --plan <plan.json>) [--library-path <dir>]... [--format json]\n"
				"Compile and execute a .mmt source file, or load and execute a .mmc artifact.\n"
				"With --plan, compile the plan's entry from exactly the plan's inputs.\n"
				"Native libraries (foreign ... from \"library\") are looked for in each\n"
				"--library-path, the plan's library_paths, MARMOT_LIBRARY_PATH, then beside\n"
				"the module that declared them.\n\n"
				"Examples:\n"
				"  marmotc run src/Main.mmt\n"
				"  marmotc run src/Main.mmc\n"
				"  marmotc src/Main.mmt\n"
				"  marmotc run src/Main.mmt --format json\n"
				"  marmotc run --plan build/plan.json\n";
		}

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
				"Usage: marmotc build (<file> | --plan <plan.json>) [--embed-sources] [--format json]\n"
				"Compile a Marmot source file and emit a .mmc binary artifact.\n"
				"With --format json, emit a .mmc.json disassembly instead.\n"
				"With --embed-sources, embed source file content in the artifact for\n"
				"richer runtime error reporting without the original .mmt on disk.\n"
				"With --plan, build the plan's entry from exactly the plan's inputs.\n\n"
				"Examples:\n"
				"  marmotc build src/Main.mmt\n"
				"  marmotc build src/Main.mmt --embed-sources\n"
				"  marmotc build src/Main.mmt --format json\n"
				"  marmotc build --plan build/plan.json\n";
		}

		if (command_name == "fmt")
		{
			return
				"Usage: marmotc fmt <file|dir> [--write|-w] [--check] [--format json]\n"
				"Format Marmot source files using the canonical CLI style.\n\n"
				"Examples:\n"
				"  marmotc fmt src/Main.mmt\n"
				"  marmotc fmt src -w\n"
				"  marmotc fmt test --check\n";
		}

		if (command_name == "test")
		{
			return
				"Usage: marmotc test [filter] [--pattern <value>] [--test <file>] [--format json]\n"
				"                    [--dir <test_dir>] [--timeout-ms <ms>] [--plan <plan.json>]\n"
				"Discover and run the tests under the test directory (default: ./test),\n"
				"each with a time limit (default: 30000 ms). With --plan, every test compiles\n"
				"from the plan's search paths and native packages; the plan needs no entry.\n"
				"`marmot test` runs this for a project, from its [test] settings.\n\n"
				"Examples:\n"
				"  marmotc test\n"
				"  marmotc test closure\n"
				"  marmotc test --pattern loop\n"
				"  marmotc test --test closure/simple.mmt\n";
		}

		return {};
	}

	[[nodiscard]] std::string GeneralHelp()
	{
		return
			"Marmot CLI\n\n"
			"Usage:\n"
			"  marmotc <file>\n"
			"  marmotc <command> [options]\n\n"
			"Commands:\n"
			"  run      Compile and execute a source file\n"
			"  check    Type-check a source file without executing it\n"
			"  build    Compile a source file and emit a bytecode artifact\n"
			"  fmt      Format one file or a directory of .mmt files\n"
			"  test     Discover and run tests\n"
			"  help     Show general or per-command help\n\n"
			"Global flags:\n"
			"  --help      Show help\n"
			"  --version   Show the Marmot version\n\n"
			"Examples:\n"
			"  marmotc fmt src -w\n"
			"  marmotc check src/Main.mmt --format json\n"
			"  marmotc run src/Main.mmt\n"
			"  marmotc run --plan plan.json\n"
			"  marmotc test closure\n\n"
			"marmotc compiles what it is given: a file, with <Name> imports found through\n"
			"MARMOT_PATH, or a build plan. Projects and packages are the marmot tool's job:\n"
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
		static constexpr std::string_view commands[] = { "run", "check", "build", "fmt", "test", "help" };
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

		if (!invocation.m_source_file.empty())
		{
			return std::unexpected(std::format("Pass either a source file or --plan to {}, not both.", command));
		}

		std::expected<BuildPlan, std::string> plan = MidoriBuildPlan::ReadFile(plan_file.value());
		if (!plan.has_value())
		{
			return std::unexpected(std::format("Invalid build plan: {}", plan.error()));
		}

		if (!plan->m_entry.has_value())
		{
			return std::unexpected(std::format("Invalid build plan: {}: {} needs an \"entry\"", plan_file->string(), command));
		}

		invocation.m_source_file = plan->m_entry.value();
		invocation.m_plan_inputs = std::move(plan->m_inputs);
		invocation.m_native_options.m_libraries = std::move(plan->m_native.m_libraries);
		invocation.m_native_options.m_search_paths.insert(
			invocation.m_native_options.m_search_paths.end(),
			plan->m_native.m_search_paths.begin(),
			plan->m_native.m_search_paths.end());
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

			if (arg == "--library-path" && kind == CommandKind::Run)
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --library-path.");
				}
				invocation.m_native_options.m_search_paths.emplace_back(args[++index]);
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
				return std::unexpected(std::format("Only one source file is allowed for {}.", kind == CommandKind::Run ? "run" : kind == CommandKind::Check ? "check" : "build"));
			}

			invocation.m_source_file = std::filesystem::path(arg);
		}

		const std::expected<void, std::string> plan_result = ApplyPlan(invocation, plan_file, kind == CommandKind::Run ? "run" : kind == CommandKind::Check ? "check" : "build");
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

			if (!invocation.m_target_path.empty())
			{
				return std::unexpected("Only one target path is allowed for fmt.");
			}

			invocation.m_target_path = std::filesystem::path(arg);
		}

		if (!invocation.m_show_help && invocation.m_target_path.empty())
		{
			return std::unexpected("Missing file or directory for fmt.");
		}

		return invocation;
	}

	[[nodiscard]] ParseResult ParseTest(const std::vector<std::string_view>& args)
	{
		Invocation invocation;
		invocation.m_kind = CommandKind::Test;

		for (size_t index = 0u; index < args.size(); index += 1u)
		{
			const std::string_view arg = args[index];
			if (arg == "-h" || arg == "--help")
			{
				invocation.m_show_help = true;
				continue;
			}

			if (arg == "--pattern")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --pattern.");
				}
				invocation.m_test_pattern = std::string(args[++index]);
				continue;
			}

			if (arg == "--test")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --test.");
				}
				invocation.m_test_file = std::string(args[++index]);
				continue;
			}

			if (arg == "--dir")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --dir.");
				}
				invocation.m_test_directory = std::filesystem::path(args[++index]);
				continue;
			}

			if (arg == "--timeout-ms")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --timeout-ms.");
				}
				const std::string_view text = args[++index];
				int timeout_ms = 0;
				const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), timeout_ms);
				if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || timeout_ms <= 0)
				{
					return std::unexpected(std::format("Invalid --timeout-ms: {}", text));
				}
				invocation.m_test_timeout_ms = timeout_ms;
				continue;
			}

			if (arg == "--plan")
			{
				if (index + 1u >= args.size())
				{
					return std::unexpected("Missing value for --plan.");
				}
				invocation.m_plan_file = std::filesystem::path(args[++index]);
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

			if (invocation.m_test_filter.has_value())
			{
				return std::unexpected("Only one positional filter is allowed for test.");
			}

			invocation.m_test_filter = std::string(arg);
		}

		return invocation;
	}

	[[nodiscard]] ParseResult ParseTestWorker(const std::vector<std::string_view>& args)
	{
		if (args.size() < 2u || args.size() > 4u)
		{
			return std::unexpected("Usage: marmotc __test-worker <test_file> <result_dir> [test_dir [plan]]");
		}

		Invocation invocation;
		invocation.m_kind = CommandKind::TestWorker;
		invocation.m_source_file = std::filesystem::path(args[0u]);
		invocation.m_worker_result_directory = std::filesystem::path(args[1u]);
		if (args.size() >= 3u)
		{
			invocation.m_target_path = std::filesystem::path(args[2u]);
		}
		if (args.size() == 4u)
		{
			invocation.m_plan_file = std::filesystem::path(args[3u]);
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
			if (rest[0] == "run")
			{
				invocation.m_kind = CommandKind::Run;
			}
			else if (rest[0] == "check")
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
			else if (rest[0] == "test")
			{
				invocation.m_kind = CommandKind::Test;
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
		if (head == "run")
		{
			parsed = ParseCompileLike(CommandKind::Run, rest);
		}
		else if (head == "check")
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
		else if (head == "test")
		{
			parsed = ParseTest(rest);
		}
		else if (head == "__test-worker")
		{
			parsed = ParseTestWorker(rest);
		}
		else
		{
			if (!head.empty() && head.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", head));
			}

			parsed = ParseCompileLike(CommandKind::Run, args);
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

	[[nodiscard]] std::string SerializeRunReportJson(const MidoriResult::CompilerReport& report, const RuntimeError* runtime_error)
	{
		const std::string warnings_json = report.Warnings().MachineReadableJson();
		const std::string compiler_errors_json = report.Errors().MachineReadableJson();
		const std::string runtime_error_json = runtime_error != nullptr ? SerializeMachineReadableRuntimeError(*runtime_error) : std::string();

		std::string errors_json = compiler_errors_json;
		if (runtime_error != nullptr)
		{
			if (errors_json == "[]")
			{
				errors_json = "[" + runtime_error_json + "]";
			}
			else
			{
				errors_json.pop_back();
				errors_json.push_back(',');
				errors_json += runtime_error_json;
				errors_json.push_back(']');
			}
		}

		std::string diagnostics_json = "[";
		const std::vector<CompilerWarning>& warnings = report.Warnings().Warnings();
		for (size_t index = 0u; index < warnings.size(); index += 1u)
		{
			if (index > 0u)
			{
				diagnostics_json.push_back(',');
			}
			diagnostics_json += SerializeMachineReadableWarningPayload(warnings[index]);
		}

		const std::vector<CompilerError>& errors = report.Errors().Errors();
		for (size_t index = 0u; index < errors.size(); index += 1u)
		{
			if (diagnostics_json.size() > 1u)
			{
				diagnostics_json.push_back(',');
			}
			diagnostics_json += SerializeMachineReadableError(errors[index]);
		}

		if (runtime_error != nullptr)
		{
			if (diagnostics_json.size() > 1u)
			{
				diagnostics_json.push_back(',');
			}
			diagnostics_json += runtime_error_json;
		}
		diagnostics_json.push_back(']');

		return std::string("{\"version\":1,\"source\":\"marmot\",\"diagnostics\":") + diagnostics_json
			+ ",\"warnings\":" + warnings_json
			+ ",\"errors\":" + errors_json + "}";
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

		if (invocation.m_format == OutputFormat::Json)
		{
			// --format json: emit the existing .mmc.json disassembly format
			const std::expected<BuildArtifactResult, std::string> artifact_result =
				WriteBuildArtifact(executable, invocation.m_source_file);
			if (!artifact_result.has_value())
			{
				MidoriResult::CompilerReport report = compiled_program.Report();
				report.AppendErrors(MidoriResult::CompilerDiagnostics(
					CompilerError::Simple(CompilerStage::Compiler, artifact_result.error())));
				std::print("{}", CommandJson("build", false, report, EXIT_FAILURE));
				return EXIT_FAILURE;
			}

			std::print("{}", CommandJson("build", true, compiled_program.Report(), EXIT_SUCCESS, {}, {}, artifact_result->m_json));
			return EXIT_SUCCESS;
		}

		// Default: write .mmc binary artifact
		std::filesystem::path artifact_path = invocation.m_source_file;
		artifact_path.replace_extension(".mmc");

		const std::expected<void, std::string> write_result =
			MidoriBinaryArtifact::WriteExecutableToFile(executable, artifact_path, invocation.m_embed_sources);
		if (!write_result.has_value())
		{
			MidoriResult::CompilerReport report = compiled_program.Report();
			report.AppendErrors(MidoriResult::CompilerDiagnostics(
				CompilerError::Simple(CompilerStage::Compiler, write_result.error())));
			std::print("{}", report.Rendered());
			return EXIT_FAILURE;
		}

		std::print("{}", compiled_program.Report().RenderedWarnings());
		std::print(
			"Built {} -> {} (procedures={}, globals={}, strings={})\n",
			invocation.m_source_file.string(),
			artifact_path.string(),
			executable.GetProcedureCount(),
			executable.GetGlobalVariableCount(),
			executable.GetStringPool().size());
		return EXIT_SUCCESS;
	}

	int HandleRun(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("run"));
			return EXIT_SUCCESS;
		}

		const MidoriBuild::ScopedTestModeOverride suppress_internal_diagnostics(true);

		NativeLibraryOptions native_options = invocation.m_native_options;
		const std::vector<std::filesystem::path> environment_library_paths = MidoriDriver::EnvironmentLibraryPaths();
		native_options.m_search_paths.insert(native_options.m_search_paths.end(), environment_library_paths.begin(), environment_library_paths.end());

		if (!invocation.m_plan_inputs.has_value() && invocation.m_source_file.extension() == ".mmc")
		{
			// Load-and-run path for pre-built binary artifacts
			MidoriDriver::LoadArtifactResult load_result = MidoriDriver::LoadArtifact(invocation.m_source_file);
			if (load_result.has_value())
			{
				std::expected<void, MidoriDriver::DriverError> native_result = MidoriDriver::LoadNativeLibraries(load_result.value(), native_options);
				if (!native_result.has_value())
				{
					load_result = std::unexpected(std::move(native_result.error()));
				}
			}
			if (!load_result.has_value())
			{
				const MidoriResult::CompilerReport report = WrapDriverErrorAsReport(load_result.error());
				if (invocation.m_format == OutputFormat::Json)
				{
					std::print("{}", CommandJson("run", false, report, EXIT_FAILURE));
				}
				else
				{
					PrintCliError(load_result.error().m_message);
				}
				return EXIT_FAILURE;
			}

			MidoriResult::CompilerReport empty_report;
			if (invocation.m_format == OutputFormat::Json)
			{
				MidoriUtility::OutputCapture capture;
				MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(load_result.value()));
				MidoriUtility::CapturedOutput captured_output = capture.Stop();
				if (!run_result.has_value())
				{
					const RuntimeError runtime_error = run_result.error();
					const std::string runtime_report_json = SerializeRunReportJson(empty_report, &runtime_error);
					std::print("{}", CommandJson("run", false, empty_report, runtime_error.ExitCode(), captured_output.m_stdout, captured_output.m_stderr, std::nullopt, runtime_report_json));
					return runtime_error.ExitCode();
				}

				std::print("{}", CommandJson("run", run_result.value() == 0, empty_report, run_result.value(), captured_output.m_stdout, captured_output.m_stderr));
				return run_result.value() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
			}

			MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(load_result.value()));
			if (!run_result.has_value())
			{
				std::print("{}", run_result.error().Rendered());
				return run_result.error().ExitCode();
			}

			return run_result.value();
		}

		// Compile-and-run path for .mmt source files. A native library that fails
		// to load is reported like a compile error: the program never starts.
		MidoriDriver::CompileFileWithReportResult compile_result = CompileInvocation(invocation);
		if (compile_result.has_value())
		{
			std::expected<void, MidoriDriver::DriverError> load_result = MidoriDriver::LoadNativeLibraries(compile_result->m_executable, native_options);
			if (!load_result.has_value())
			{
				compile_result = std::unexpected(std::move(load_result.error()));
			}
		}
		if (!compile_result.has_value())
		{
			const MidoriResult::CompilerReport report = WrapDriverErrorAsReport(compile_result.error());
			if (invocation.m_format == OutputFormat::Json)
			{
				std::print("{}", CommandJson("run", false, report, EXIT_FAILURE));
			}
			else
			{
				std::print("{}", compile_result.error().Rendered());
			}
			return EXIT_FAILURE;
		}

		MidoriResult::CompiledProgram compiled_program = std::move(compile_result).value();
		MidoriResult::CompilerReport report = compiled_program.Report();
		if (invocation.m_format == OutputFormat::Json)
		{
			MidoriUtility::OutputCapture capture;
			MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(compiled_program).TakeExecutable());
			MidoriUtility::CapturedOutput captured_output = capture.Stop();
			if (!run_result.has_value())
			{
				const RuntimeError runtime_error = run_result.error();
				const std::string runtime_report_json = SerializeRunReportJson(report, &runtime_error);
				std::print("{}", CommandJson("run", false, report, runtime_error.ExitCode(), captured_output.m_stdout, captured_output.m_stderr, std::nullopt, runtime_report_json));
				return runtime_error.ExitCode();
			}

			std::print("{}", CommandJson("run", run_result.value() == 0, report, run_result.value(), captured_output.m_stdout, captured_output.m_stderr));
			return run_result.value() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
		}

		std::print("{}", report.RenderedWarnings());
		if (ShouldEmitMachineReadableWarnings())
		{
			std::print("{}", report.MachineReadableWarnings());
		}
		MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(compiled_program).TakeExecutable());
		if (!run_result.has_value())
		{
			std::print("{}", run_result.error().Rendered());
			return run_result.error().ExitCode();
		}

		return run_result.value();
	}

	int HandleFmt(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("fmt"));
			return EXIT_SUCCESS;
		}

		std::error_code error_code;
		const bool is_directory = std::filesystem::is_directory(invocation.m_target_path, error_code);
		if (is_directory && !invocation.m_fmt_write && !invocation.m_fmt_check)
		{
			PrintCliError("Formatting a directory requires --write or --check.");
			return EXIT_FAILURE;
		}

		if (!is_directory && !std::filesystem::is_regular_file(invocation.m_target_path, error_code))
		{
			PrintCliError(std::format("Could not find target for fmt: {}", invocation.m_target_path.string()));
			return EXIT_FAILURE;
		}

		if (!is_directory && !invocation.m_fmt_write && !invocation.m_fmt_check)
		{
			const std::expected<std::string, std::string> read_result = [&]() -> std::expected<std::string, std::string>
			{
				std::ifstream input(invocation.m_target_path, std::ios::binary);
				if (!input.is_open())
				{
					return std::unexpected(std::format("Could not open file: {}", invocation.m_target_path.string()));
				}

				std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
				return text;
			}();
			if (!read_result.has_value())
			{
				PrintCliError(read_result.error());
				return EXIT_FAILURE;
			}

			const std::expected<std::string, CompilerError> format_result = MidoriFormatter::FormatSource(read_result.value(), invocation.m_target_path.string());
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
				MidoriJson::AppendStringField(payload, "target", invocation.m_target_path.generic_string(), first_field);
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

		const MidoriFormatter::RunResult result = MidoriFormatter::FormatPath(
			invocation.m_target_path,
			MidoriFormatter::Options{ invocation.m_fmt_write, invocation.m_fmt_check });

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

	int HandleTest(const Invocation& invocation)
	{
		if (invocation.m_show_help)
		{
			std::print("{}", CommandHelp("test"));
			return EXIT_SUCCESS;
		}

		std::expected<MidoriTestRunner::Options, std::string> options = MidoriTestRunner::Options::Create(
			std::filesystem::current_path(),
			invocation.m_test_directory,
			invocation.m_test_timeout_ms,
			invocation.m_plan_file);
		if (!options.has_value())
		{
			PrintCliError(options.error());
			return EXIT_FAILURE;
		}
		options->m_filter = invocation.m_test_filter;
		options->m_pattern = invocation.m_test_pattern;
		options->m_test_file = invocation.m_test_file;

		MidoriTestRunner::RunResult result = MidoriTestRunner::Run(options.value());

		if (invocation.m_format == OutputFormat::Json)
		{
			std::print("{}", result.MachineReadableJson());
		}
		else if (result.TotalCount() == 0)
		{
			std::print("No tests found in {}\n", result.m_test_directory.string());
		}
		else
		{
			std::print("{}", result.Rendered());
		}

		return result.Succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	int HandleTestWorker(const Invocation& invocation)
	{
		return MidoriTestRunner::RunWorker(
			MidoriTestRunner::WorkerOptions
			{
				.m_test_path = invocation.m_source_file,
				.m_result_directory = invocation.m_worker_result_directory,
				.m_test_directory = invocation.m_target_path,
				.m_plan_file = invocation.m_plan_file
			});
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
			{ "run", "Compile and execute a source file", CommandKind::Run, &HandleRun },
			{ "check", "Type-check a source file without executing it", CommandKind::Check, &HandleCheck },
			{ "build", "Compile a source file and report bytecode stats", CommandKind::Build, &HandleBuild },
			{ "fmt", "Format one file or a directory of .mmt files", CommandKind::Fmt, &HandleFmt },
			{ "test", "Discover and run tests", CommandKind::Test, &HandleTest }
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
		if (kind == CommandKind::TestWorker)
		{
			return &HandleTestWorker;
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
