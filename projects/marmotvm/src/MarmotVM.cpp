// marmotvm: runs a compiled Marmot program (.mmc). It compiles nothing; marmotc
// writes the program and the marmot tool runs both.

#include "Support/Diagnostics/Diagnostics.h"
#include "Bytecode/Disassembler/Disassembler.h"
#include "Error/RuntimeError.h"
#include "Support/Json/Json.h"
#include "Support/OutputCapture/OutputCapture.h"
#include "Support/Terminal/Terminal.h"
#include "Library/ProgramArguments/ProgramArguments.h"
#include "Support/TextPath.h"
#include "Loader/ProgramLoader.h"
#include "Loader/Standalone.h"

#include <algorithm>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <CLI/CLI.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
	constexpr std::string_view USAGE =
		"Usage: marmotvm run <program.mmc> [--library <name>=<file>]... [--library-path <dir>]...\n"
		"                [--format json] [-- <argument>...]\n"
		"       marmotvm disassemble <program.mmc>\n"
#if MARMOT_ENABLE_OPCODE_METRICS
		"Run diagnostics: --opcode-metrics, --gc-metrics (stderr).\n"
#endif
#if MARMOT_ENABLE_EXECUTION_TRACE
		"                --trace (instructions and stack on stderr).\n"
#endif
		"Run a compiled Marmot program.\n\n"
		"A native library the program names (foreign ... from \"name\") is looked for\n"
		"at its --library file, then in each --library-path, then in MARMOT_LIBRARY_PATH,\n"
		"then beside the module that declared it (lib/<platform>/, then the directory).\n"
		"Everything after -- goes to the program, which reads it with System::Arguments.\n\n"
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
		std::vector<std::string> m_program_arguments;
		MarmotProgramLoader::NativeLibraryLocations m_locations;
		bool m_json = false;
		bool m_show_help = false;
		bool m_show_version = false;
		bool m_standalone_version = false;
		bool m_trace = false;
		bool m_opcode_metrics = false;
		bool m_gc_metrics = false;
	};

	// Everything after the executable's own name, as UTF-8 text. Windows hands
	// main its arguments in the code page, so there they come from the wide
	// command line instead, through a path, which is how UTF-16 becomes text.
	[[nodiscard]] std::vector<std::string> Utf8Arguments([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
	{
#ifdef _WIN32
		int count = 0;
		LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
		std::vector<std::string> arguments = std::views::iota(1, count)
			| std::views::transform([wide](int index) { return TextPath::ToText(std::filesystem::path(wide[index])); })
			| std::ranges::to<std::vector>();
		LocalFree(wide);
		return arguments;
#else
		return std::vector<std::string>(argv + 1, argv + argc);
#endif
	}

	[[nodiscard]] std::expected<Invocation, std::string> Parse(std::span<const std::string> arguments)
	{
		Invocation invocation;
		std::string command;
		std::vector<std::string> formats;
		std::vector<std::string> programs;
		std::vector<std::string> library_paths;
		std::vector<std::string> libraries;

		const auto separator = std::ranges::find(arguments, "--");
		if (separator != arguments.end())
		{
			invocation.m_program_arguments.assign(separator + 1, arguments.end());
		}
		// CLI11 consumes vectors from the back.
		std::vector<std::string> options(std::make_reverse_iterator(separator), std::make_reverse_iterator(arguments.begin()));

		CLI::App app;
		app.set_help_flag();
		app.add_flag("-h,--help", invocation.m_show_help);
		app.add_flag("--version", invocation.m_show_version);
		app.add_flag("--standalone-version", invocation.m_standalone_version);
		app.add_flag("--trace", invocation.m_trace);
		app.add_flag("--opcode-metrics", invocation.m_opcode_metrics);
		app.add_flag("--gc-metrics", invocation.m_gc_metrics);
		app.add_option("--format", formats)->expected(1)->take_all();
		app.add_option("--library-path", library_paths)->expected(1)->take_all();
		app.add_option("--library", libraries)->expected(1)->take_all();
		app.add_option("command", command);
		app.add_option("program", programs);

		try
		{
			app.parse(options);
		}
		catch (const CLI::ExtrasError& error)
		{
			const std::vector<std::string> extras = app.remaining();
			return std::unexpected(extras.empty() ? std::string(error.what()) : "Unknown option: " + extras.front());
		}
		catch (const CLI::ArgumentMismatch& error)
		{
			const std::string_view message = error.what();
			if (message.starts_with("--library-path"))
			{
				return std::unexpected("Missing value for --library-path.");
			}
			if (message.starts_with("--library"))
			{
				return std::unexpected("--library takes <name>=<file>.");
			}
			if (message.starts_with("--format"))
			{
				return std::unexpected("--format takes 'json'.");
			}
			return std::unexpected(error.what());
		}
		catch (const CLI::ParseError& error)
		{
			return std::unexpected(error.what());
		}

#if !MARMOT_ENABLE_EXECUTION_TRACE
		if (invocation.m_trace)
		{
			return std::unexpected("--trace requires a Debug build.");
		}
#endif
#if !MARMOT_ENABLE_OPCODE_METRICS
		if (invocation.m_opcode_metrics || invocation.m_gc_metrics)
		{
			return std::unexpected(std::format("{} requires a Debug or Dev build.", invocation.m_opcode_metrics ? "--opcode-metrics" : "--gc-metrics"));
		}
#endif
		if (!std::ranges::all_of(formats, [](const std::string& format) { return format == "json"; }))
		{
			return std::unexpected("--format takes 'json'.");
		}
		invocation.m_json = !formats.empty();

		for (const std::string& directory : library_paths)
		{
			invocation.m_locations.m_search_paths.emplace_back(TextPath::FromText(directory));
		}
		for (std::string_view library : libraries)
		{
			const size_t equals = library.find('=');
			if (equals == std::string_view::npos || equals == 0u || equals + 1u == library.size())
			{
				return std::unexpected("--library takes <name>=<file>.");
			}
			invocation.m_locations.m_files.insert_or_assign(std::string(library.substr(0u, equals)), TextPath::FromText(library.substr(equals + 1u)));
		}

		if (command == "run")
		{
			invocation.m_command = CommandKind::Run;
		}
		else if (command == "disassemble")
		{
			invocation.m_command = CommandKind::Disassemble;
		}
		else if (!command.empty())
		{
			return std::unexpected(std::format("Unknown command: {}. Use run or disassemble.", command));
		}

		if (programs.size() > 1u)
		{
			return std::unexpected("marmotvm runs one program.");
		}
		if (!programs.empty())
		{
			invocation.m_program = TextPath::FromText(programs.front());
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
			 !invocation.m_locations.m_files.empty() || !invocation.m_locations.m_search_paths.empty() || !invocation.m_program_arguments.empty()))
		{
			return std::unexpected("disassemble takes only a .mmc file; execution options belong to run.");
		}
		return invocation;
	}

	// The same shape as marmotc's reports: one diagnostic, as both the only
	// diagnostic and the only error.
	[[nodiscard]] MarmotJson::Document ReportJson(const std::optional<std::string>& error_json)
	{
		MarmotJson::Document errors = MarmotJson::Document::Array();
		if (error_json)
		{
			errors.AppendRaw(*error_json);
		}
		MarmotJson::Document report;
		report.AddNumberField("version", 1);
		report.AddStringField("source", "marmot");
		report.AddField("diagnostics", errors);
		report.AddField("warnings", MarmotJson::Document::Array());
		report.AddField("errors", errors);
		return report;
	}

	[[nodiscard]] std::string RunJson(bool success, int exit_code, std::string_view stdout_text, std::string_view stderr_text, const std::optional<std::string>& error_json)
	{
		MarmotJson::Document payload;
		payload.AddNumberField("version", 1);
		payload.AddStringField("source", "marmotvm");
		payload.AddStringField("command", "run");
		payload.AddBoolField("success", success);
		payload.AddNumberField("exitCode", exit_code);
		payload.AddStringField("stdout", stdout_text);
		payload.AddStringField("stderr", stderr_text);
		payload.AddField("report", ReportJson(error_json));

		return payload.Write();
	}

	// The same fields as marmotc's diagnostics, reported as a Module error.
	[[nodiscard]] std::string StartFailureJson(std::string_view message)
	{
		const std::optional<std::string_view> no_text = std::nullopt;
		const std::optional<int> no_number = std::nullopt;
		MarmotJson::Document payload;
		payload.AddStringField("source", "marmot");
		payload.AddStringField("severity", "error");
		payload.AddStringField("stage", "Module");
		payload.AddStringField("code", "None");
		payload.AddStringField("message", message);
		payload.AddStringField("file", no_text);
		payload.AddNumberField("line", no_number);
		payload.AddNumberField("column", no_number);
		payload.AddNumberField("endLine", no_number);
		payload.AddNumberField("endColumn", no_number);
		payload.AddNumberField("caret_length", no_number);
		payload.AddStringField("suggestion", no_text);
		payload.AddField("relatedInformation", MarmotJson::Document::Array());

		return payload.Write();
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
		std::expected<VmExecutable, std::string> program = MarmotProgramLoader::ReadProgram(invocation.m_program);
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

		MarmotProgramLoader::NativeLibraryLocations locations = invocation.m_locations;
		for (const std::filesystem::path& directory : MarmotProgramLoader::EnvironmentLibraryPaths())
		{
			locations.m_search_paths.push_back(directory);
		}
		const std::expected<void, std::string> loaded = MarmotProgramLoader::LoadNativeLibraries(program.value(), locations);
		if (!loaded.has_value())
		{
			return FailToStart(invocation, loaded.error());
		}

		if (invocation.m_json)
		{
			const RuntimeDiagnostics::ScopedOutput diagnostic_output;
			MarmotUtility::OutputCapture capture;
			const std::expected<int, RuntimeError> run_result = MarmotProgramLoader::Run(std::move(program).value());
			const MarmotUtility::CapturedOutput output = capture.Stop();
			if (!run_result.has_value())
			{
				std::print("{}", RunJson(false, run_result.error().ExitCode(), output.m_stdout, output.m_stderr, SerializeMachineReadableRuntimeError(run_result.error())));
				return run_result.error().ExitCode();
			}

			std::print("{}", RunJson(run_result.value() == 0, run_result.value(), output.m_stdout, output.m_stderr, std::nullopt));
			return run_result.value();
		}

		const std::expected<int, RuntimeError> run_result = MarmotProgramLoader::Run(std::move(program).value());
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
	MarmotTerminal::SetColorEnabled(MarmotTerminal::ShouldUseColor(stdout));
	std::vector<std::string> arguments = Utf8Arguments(argc, argv);
	std::expected<std::optional<MarmotStandalone::Package>, std::string> embedded =
		MarmotStandalone::ReadCurrentExecutable();
	if (!embedded.has_value())
	{
		std::print(stderr, "{}\n", embedded.error());
		return EXIT_FAILURE;
	}
	if (embedded->has_value())
	{
		ProgramArguments::Set(std::move(arguments));
		const std::expected<int, std::string> result = MarmotStandalone::Run(std::move(embedded->value()));
		if (!result.has_value())
		{
			std::print(stderr, "{}\n", result.error());
			return EXIT_FAILURE;
		}
		return result.value();
	}
	const std::expected<Invocation, std::string> invocation = Parse(arguments);
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
		std::print("marmotvm {}\n", MARMOT_VERSION_STRING);
		return EXIT_SUCCESS;
	}
	if (invocation->m_standalone_version)
	{
		const std::expected<std::filesystem::path, std::string> path = MarmotStandalone::CurrentExecutablePath();
		if (!path.has_value())
		{
			std::print(stderr, "{}\n", path.error());
			return EXIT_FAILURE;
		}
		const std::u8string utf8_path = path->generic_u8string();
		MarmotJson::Document payload;
		payload.AddNumberField("version", MarmotStandalone::s_format_version);
		payload.AddStringField("executable", std::string(utf8_path.begin(), utf8_path.end()));
		std::print("{}\n", payload.Write());
		return EXIT_SUCCESS;
	}

	ProgramArguments::Set(invocation->m_program_arguments);
	RuntimeDiagnostics::Configure(invocation->m_trace, invocation->m_opcode_metrics, invocation->m_gc_metrics);
	return Execute(invocation.value());
}
