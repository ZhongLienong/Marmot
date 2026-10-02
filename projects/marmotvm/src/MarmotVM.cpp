// marmotvm: runs a compiled Marmot program (.mmc). It compiles nothing; marmotc
// writes the program and the marmot tool runs both.

#include "Support/Diagnostics/Diagnostics.h"
#include "Bytecode/Disassembler/Disassembler.h"
#include "Error/RuntimeError.h"
#include "Support/Json/Json.h"
#include "Support/OutputCapture/OutputCapture.h"
#include "Loader/ProgramLoader.h"
#include "Loader/Standalone.h"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	constexpr std::string_view USAGE =
		"Usage: marmotvm run <program.mmc> [--library <name>=<file>]... [--library-path <dir>]...\n"
		"                [--format json]\n"
		"       marmotvm disassemble <program.mmc>\n"
#if MIDORI_ENABLE_OPCODE_METRICS
		"Run diagnostics: --opcode-metrics, --gc-metrics (stderr).\n"
#endif
#if MIDORI_ENABLE_EXECUTION_TRACE
		"                --trace (instructions and stack on stderr).\n"
#endif
		"Run a compiled Marmot program.\n\n"
		"A native library the program names (foreign ... from \"name\") is looked for\n"
		"at its --library file, then in each --library-path, then in MARMOT_LIBRARY_PATH,\n"
		"then beside the module that declared it (lib/<platform>/, then the directory).\n\n"
		"Examples:\n"
		"  marmotvm run target/Main.mmc\n"
		"  marmotvm disassemble target/Main.mmc\n"
		"  marmotvm run Main.mmc --library-path native/bin\n"
		"  marmotvm run Main.mmc --library marmot_image=bin/marmot_image.dll\n";

	enum class CommandKind
	{
		Run,
		Disassemble
	};

	struct Invocation
	{
		std::optional<CommandKind> m_command;
		std::filesystem::path m_program;
		MidoriProgramLoader::NativeLibraryLocations m_locations;
		bool m_json = false;
		bool m_show_help = false;
		bool m_show_version = false;
		bool m_standalone_version = false;
		bool m_trace = false;
		bool m_opcode_metrics = false;
		bool m_gc_metrics = false;
	};

	// The value after the option at `index`, which moves past it.
	[[nodiscard]] std::optional<std::string_view> TakeValue(int argc, char* argv[], int& index)
	{
		if (index + 1 >= argc)
		{
			return std::nullopt;
		}
		index += 1;
		return std::string_view(argv[index]);
	}

	[[nodiscard]] std::expected<Invocation, std::string> Parse(int argc, char* argv[])
	{
		Invocation invocation;
		for (int index = 1; index < argc; index += 1)
		{
			const std::string_view arg = argv[index];

			if (arg == "-h" || arg == "--help")
			{
				invocation.m_show_help = true;
			}
			else if (arg == "--version")
			{
				invocation.m_show_version = true;
			}
			else if (arg == "--standalone-version")
			{
				invocation.m_standalone_version = true;
			}
			else if (arg == "--trace")
			{
#if MIDORI_ENABLE_EXECUTION_TRACE
				invocation.m_trace = true;
#else
				return std::unexpected("--trace requires a Debug build.");
#endif
			}
			else if (arg == "--opcode-metrics" || arg == "--gc-metrics")
			{
#if MIDORI_ENABLE_OPCODE_METRICS
				if (arg == "--opcode-metrics")
				{
					invocation.m_opcode_metrics = true;
				}
				else
				{
					invocation.m_gc_metrics = true;
				}
#else
				return std::unexpected(std::format("{} requires a Debug or Dev build.", arg));
#endif
			}
			else if (arg == "--format")
			{
				const std::optional<std::string_view> value = TakeValue(argc, argv, index);
				if (!value.has_value() || value.value() != "json")
				{
					return std::unexpected("--format takes 'json'.");
				}
				invocation.m_json = true;
			}
			else if (arg == "--library-path")
			{
				const std::optional<std::string_view> value = TakeValue(argc, argv, index);
				if (!value.has_value())
				{
					return std::unexpected("Missing value for --library-path.");
				}
				invocation.m_locations.m_search_paths.emplace_back(value.value());
			}
			else if (arg == "--library")
			{
				const std::optional<std::string_view> value = TakeValue(argc, argv, index);
				const size_t equals = value.has_value() ? value->find('=') : std::string_view::npos;
				if (equals == std::string_view::npos || equals == 0u || equals + 1u == value->size())
				{
					return std::unexpected("--library takes <name>=<file>.");
				}
				invocation.m_locations.m_files.insert_or_assign(std::string(value->substr(0u, equals)), std::filesystem::path(value->substr(equals + 1u)));
			}
			else if (!arg.empty() && arg.front() == '-')
			{
				return std::unexpected(std::format("Unknown option: {}", arg));
			}
			else if (!invocation.m_command.has_value())
			{
				if (arg == "run")
				{
					invocation.m_command = CommandKind::Run;
				}
				else if (arg == "disassemble")
				{
					invocation.m_command = CommandKind::Disassemble;
				}
				else
				{
					return std::unexpected(std::format("Unknown command: {}. Use run or disassemble.", arg));
				}
			}
			else if (!invocation.m_program.empty())
			{
				return std::unexpected("marmotvm runs one program.");
			}
			else
			{
				invocation.m_program = std::filesystem::path(arg);
			}
		}

		if (invocation.m_show_help || invocation.m_show_version || invocation.m_standalone_version)
		{
			return invocation;
		}
		if (!invocation.m_command.has_value())
		{
			return std::unexpected("Missing command. Use run or disassemble.");
		}
		if (invocation.m_program.empty())
		{
			return std::unexpected("Missing program (a .mmc file).");
		}
		if (invocation.m_command == CommandKind::Disassemble &&
			(invocation.m_json || invocation.m_trace || invocation.m_opcode_metrics || invocation.m_gc_metrics ||
			 !invocation.m_locations.m_files.empty() || !invocation.m_locations.m_search_paths.empty()))
		{
			return std::unexpected("disassemble takes only a .mmc file; execution options belong to run.");
		}
		return invocation;
	}

	// The same shape as marmotc's reports: one diagnostic, as both the only
	// diagnostic and the only error.
	[[nodiscard]] std::string ReportJson(const std::optional<std::string>& error_json)
	{
		const std::string errors = error_json.has_value() ? "[" + error_json.value() + "]" : "[]";
		return "{\"version\":1,\"source\":\"marmot\",\"diagnostics\":" + errors + ",\"warnings\":[],\"errors\":" + errors + "}";
	}

	[[nodiscard]] std::string RunJson(bool success, int exit_code, std::string_view stdout_text, std::string_view stderr_text, const std::optional<std::string>& error_json)
	{
		std::string payload = "{";
		bool first_field = true;
		RuntimeJson::AppendNumberField(payload, "version", 1, first_field);
		RuntimeJson::AppendStringField(payload, "source", "marmotvm", first_field);
		RuntimeJson::AppendStringField(payload, "command", "run", first_field);
		RuntimeJson::AppendBoolField(payload, "success", success, first_field);
		RuntimeJson::AppendNumberField(payload, "exitCode", exit_code, first_field);
		RuntimeJson::AppendStringField(payload, "stdout", stdout_text, first_field);
		RuntimeJson::AppendStringField(payload, "stderr", stderr_text, first_field);
		RuntimeJson::AppendRawField(payload, "report", ReportJson(error_json), first_field);
		payload.push_back('}');
		return payload;
	}

	// The same fields as marmotc's diagnostics, reported as a Module error.
	[[nodiscard]] std::string StartFailureJson(std::string_view message)
	{
		const std::optional<std::string_view> no_text = std::nullopt;
		const std::optional<int> no_number = std::nullopt;
		std::string payload = "{";
		bool first_field = true;
		RuntimeJson::AppendStringField(payload, "source", "marmot", first_field);
		RuntimeJson::AppendStringField(payload, "severity", "error", first_field);
		RuntimeJson::AppendStringField(payload, "stage", "Module", first_field);
		RuntimeJson::AppendStringField(payload, "code", "None", first_field);
		RuntimeJson::AppendStringField(payload, "message", message, first_field);
		RuntimeJson::AppendStringField(payload, "file", no_text, first_field);
		RuntimeJson::AppendStringField(payload, "file_path", no_text, first_field);
		RuntimeJson::AppendNumberField(payload, "line", no_number, first_field);
		RuntimeJson::AppendNumberField(payload, "column", no_number, first_field);
		RuntimeJson::AppendNumberField(payload, "endLine", no_number, first_field);
		RuntimeJson::AppendNumberField(payload, "endColumn", no_number, first_field);
		RuntimeJson::AppendNumberField(payload, "caret_length", no_number, first_field);
		RuntimeJson::AppendStringField(payload, "suggestion", no_text, first_field);
		RuntimeJson::AppendRawField(payload, "relatedInformation", "[]", first_field);
		payload.push_back('}');
		return payload;
	}

	// A program that cannot start: unreadable, or a native library did not load.
	[[nodiscard]] int FailToStart(const Invocation& invocation, const std::string& message)
	{
		if (invocation.m_json)
		{
			std::print("{}", RunJson(false, EXIT_FAILURE, {}, {}, StartFailureJson(message)));
		}
		else
		{
			std::print(stderr, "{}\n", message);
		}
		return EXIT_FAILURE;
	}

	[[nodiscard]] int Execute(const Invocation& invocation)
	{
		std::expected<VmExecutable, std::string> program = MidoriProgramLoader::ReadProgram(invocation.m_program);
		if (!program.has_value())
		{
			return FailToStart(invocation, program.error());
		}
		if (invocation.m_command == CommandKind::Disassemble)
		{
			for (int index = 0; index < program->GetProcedureCount(); index += 1)
			{
				Disassembler::DisassembleBytecodeStream(stdout, program.value(), index, std::format("Procedure {}", index));
			}
			return EXIT_SUCCESS;
		}

		MidoriProgramLoader::NativeLibraryLocations locations = invocation.m_locations;
		for (const std::filesystem::path& directory : MidoriProgramLoader::EnvironmentLibraryPaths())
		{
			locations.m_search_paths.push_back(directory);
		}
		const std::expected<void, std::string> loaded = MidoriProgramLoader::LoadNativeLibraries(program.value(), locations);
		if (!loaded.has_value())
		{
			return FailToStart(invocation, loaded.error());
		}

		if (invocation.m_json)
		{
			const RuntimeDiagnostics::ScopedOutput diagnostic_output;
			MidoriUtility::OutputCapture capture;
			const std::expected<int, RuntimeError> run_result = MidoriProgramLoader::Run(std::move(program).value());
			const MidoriUtility::CapturedOutput output = capture.Stop();
			if (!run_result.has_value())
			{
				std::print("{}", RunJson(false, run_result.error().ExitCode(), output.m_stdout, output.m_stderr, SerializeMachineReadableRuntimeError(run_result.error())));
				return run_result.error().ExitCode();
			}

			std::print("{}", RunJson(run_result.value() == 0, run_result.value(), output.m_stdout, output.m_stderr, std::nullopt));
			return run_result.value();
		}

		const std::expected<int, RuntimeError> run_result = MidoriProgramLoader::Run(std::move(program).value());
		if (!run_result.has_value())
		{
			std::print("{}", run_result.error().Rendered());
			return run_result.error().ExitCode();
		}
		return run_result.value();
	}
}

int main(int argc, char* argv[])
{
	std::expected<std::optional<MidoriStandalone::Package>, std::string> embedded =
		MidoriStandalone::ReadCurrentExecutable();
	if (!embedded.has_value())
	{
		std::print(stderr, "{}\n", embedded.error());
		return EXIT_FAILURE;
	}
	if (embedded->has_value())
	{
		const std::expected<int, std::string> result = MidoriStandalone::Run(std::move(embedded->value()));
		if (!result.has_value())
		{
			std::print(stderr, "{}\n", result.error());
			return EXIT_FAILURE;
		}
		return result.value();
	}
	const std::expected<Invocation, std::string> invocation = Parse(argc, argv);
	if (!invocation.has_value())
	{
		std::print(stderr, "{}\n\n{}", invocation.error(), USAGE);
		return EXIT_FAILURE;
	}

	if (invocation->m_show_help)
	{
		std::print("{}", USAGE);
		return EXIT_SUCCESS;
	}

	if (invocation->m_show_version)
	{
		std::print("marmotvm {}\n", MIDORI_VERSION_STRING);
		return EXIT_SUCCESS;
	}
	if (invocation->m_standalone_version)
	{
		const std::expected<std::filesystem::path, std::string> path = MidoriStandalone::CurrentExecutablePath();
		if (!path.has_value())
		{
			std::print(stderr, "{}\n", path.error());
			return EXIT_FAILURE;
		}
		const std::u8string utf8_path = path->generic_u8string();
		std::print("{{\"version\":{},\"executable\":\"{}\"}}\n",
			MidoriStandalone::s_format_version,
			RuntimeJson::EscapeString(std::string(utf8_path.begin(), utf8_path.end())));
		return EXIT_SUCCESS;
	}

	RuntimeDiagnostics::Configure(invocation->m_trace, invocation->m_opcode_metrics, invocation->m_gc_metrics);
	return Execute(invocation.value());
}
