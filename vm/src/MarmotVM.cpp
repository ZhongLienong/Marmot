// marmotvm: runs a compiled Marmot program (.mmc). It compiles nothing; marmotc
// writes the program and the marmot tool runs both.

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Error/Error.h"
#include "Common/Json/Json.h"
#include "Common/OutputCapture/OutputCapture.h"
#include "Loader/ProgramLoader.h"

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
		"Usage: marmotvm <program.mmc> [--library <name>=<file>]... [--library-path <dir>]...\n"
		"                [--format json]\n"
		"Run a compiled Marmot program.\n\n"
		"A native library the program names (foreign ... from \"name\") is looked for\n"
		"at its --library file, then in each --library-path, then in MARMOT_LIBRARY_PATH,\n"
		"then beside the module that declared it (lib/<platform>/, then the directory).\n\n"
		"Examples:\n"
		"  marmotvm target/Main.mmc\n"
		"  marmotvm Main.mmc --library-path native/bin\n"
		"  marmotvm Main.mmc --library marmot_image=bin/marmot_image.dll\n";

	struct Invocation
	{
		std::filesystem::path m_program;
		MidoriProgramLoader::NativeLibraryLocations m_locations;
		bool m_json = false;
		bool m_show_help = false;
		bool m_show_version = false;
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
			else if (!invocation.m_program.empty())
			{
				return std::unexpected("marmotvm runs one program.");
			}
			else
			{
				invocation.m_program = std::filesystem::path(arg);
			}
		}

		if (!invocation.m_show_help && !invocation.m_show_version && invocation.m_program.empty())
		{
			return std::unexpected("Missing the program to run (a .mmc file).");
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
		MidoriJson::AppendNumberField(payload, "version", 1, first_field);
		MidoriJson::AppendStringField(payload, "source", "marmotvm", first_field);
		MidoriJson::AppendStringField(payload, "command", "run", first_field);
		MidoriJson::AppendBoolField(payload, "success", success, first_field);
		MidoriJson::AppendNumberField(payload, "exitCode", exit_code, first_field);
		MidoriJson::AppendStringField(payload, "stdout", stdout_text, first_field);
		MidoriJson::AppendStringField(payload, "stderr", stderr_text, first_field);
		MidoriJson::AppendRawField(payload, "report", ReportJson(error_json), first_field);
		payload.push_back('}');
		return payload;
	}

	// A program that cannot start: unreadable, or a native library did not load.
	[[nodiscard]] int FailToStart(const Invocation& invocation, const std::string& message)
	{
		if (invocation.m_json)
		{
			const CompilerError error = CompilerError::Simple(CompilerStage::Module, message);
			std::print("{}", RunJson(false, EXIT_FAILURE, {}, {}, SerializeMachineReadableError(error)));
		}
		else
		{
			std::print(stderr, "{}\n", message);
		}
		return EXIT_FAILURE;
	}

	[[nodiscard]] int Execute(const Invocation& invocation)
	{
		std::expected<MidoriExecutable, std::string> program = MidoriProgramLoader::ReadProgram(invocation.m_program);
		if (!program.has_value())
		{
			return FailToStart(invocation, program.error());
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
		std::print("marmotvm {}\n", MidoriBuild::VersionString);
		return EXIT_SUCCESS;
	}

	const MidoriBuild::ScopedTestModeOverride suppress_internal_diagnostics(true);
	return Execute(invocation.value());
}
