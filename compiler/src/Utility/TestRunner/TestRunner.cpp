#include "Utility/TestRunner/TestRunner.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <sstream>
#include <system_error>

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Json/Json.h"
#include "Utility/Driver/MidoriDriver.h"
#include "Utility/OutputCapture/OutputCapture.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <csignal>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	constexpr std::string_view Green = "\033[92m";
	constexpr std::string_view Red = "\033[91m";
	constexpr std::string_view Yellow = "\033[93m";
	constexpr std::string_view Blue = "\033[94m";
	constexpr std::string_view Cyan = "\033[96m";
	constexpr std::string_view Gray = "\033[90m";
	constexpr std::string_view Bold = "\033[1m";
	constexpr std::string_view Reset = "\033[0m";

	[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open())
		{
			return {};
		}

		std::ostringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

	bool WriteTextFile(const std::filesystem::path& path, std::string_view text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			return false;
		}

		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		return static_cast<bool>(output);
	}

	[[nodiscard]] std::string EmptyReportJson()
	{
		return "{\"version\":1,\"source\":\"marmot\",\"diagnostics\":[],\"warnings\":[],\"errors\":[]}";
	}

	[[nodiscard]] std::filesystem::path MakeWorkerResultDirectory()
	{
		const std::filesystem::path base = std::filesystem::temp_directory_path();
		for (int index = 0; index < 32; index += 1)
		{
			const std::filesystem::path candidate = base / std::format(
				"marmot-test-worker-{}-{}",
				static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()),
				index);
			std::error_code error_code;
			if (std::filesystem::create_directories(candidate, error_code))
			{
				return candidate;
			}
		}

		return {};
	}

	[[nodiscard]] std::filesystem::path CurrentExecutablePath()
	{
#ifdef _WIN32
		std::wstring buffer(static_cast<size_t>(MAX_PATH), L'\0');
		while (true)
		{
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0u)
			{
				return {};
			}

			if (length < buffer.size() - 1u)
			{
				buffer.resize(static_cast<size_t>(length));
				return std::filesystem::path(buffer);
			}

			buffer.resize(buffer.size() * 2u);
		}
#else
#if defined(__APPLE__)
		uint32_t size = 0u;
		_NSGetExecutablePath(nullptr, &size);
		std::vector<char> buffer(static_cast<size_t>(size), '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		{
			return {};
		}

		std::error_code error_code;
		return std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), error_code);
#else
		std::vector<char> buffer(static_cast<size_t>(PATH_MAX), '\0');
		while (true)
		{
			const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size());
			if (length < 0)
			{
				return {};
			}

			if (static_cast<size_t>(length) < buffer.size())
			{
				return std::filesystem::path(std::string(buffer.data(), static_cast<size_t>(length)));
			}

			buffer.resize(buffer.size() * 2u);
		}
#endif
#endif
	}

	struct ChildProcess
	{
#ifdef _WIN32
		PROCESS_INFORMATION m_process_information{};
#else
		pid_t m_pid = -1;
#endif
		bool m_started = false;
	};

#ifdef _WIN32
	[[nodiscard]] std::wstring QuoteWindowsArgument(const std::filesystem::path& path)
	{
		const std::wstring value = path.wstring();
		std::wstring quoted = L"\"";
		quoted.reserve(value.size() + 2u);
		for (wchar_t ch : value)
		{
			if (ch == L'\"')
			{
				quoted += L"\\\"";
			}
			else
			{
				quoted.push_back(ch);
			}
		}
		quoted.push_back(L'\"');
		return quoted;
	}
#endif

	[[nodiscard]] std::optional<ChildProcess> StartWorkerProcess(
		const std::filesystem::path& executable_path,
		const std::filesystem::path& test_path,
		const std::filesystem::path& result_directory,
		const std::filesystem::path& test_directory)
	{
#ifdef _WIN32
		STARTUPINFOW startup_info{};
		startup_info.cb = sizeof(startup_info);
		PROCESS_INFORMATION process_information{};

		std::wstring command_line = QuoteWindowsArgument(executable_path);
		command_line += L" __test-worker ";
		command_line += QuoteWindowsArgument(test_path);
		command_line += L" ";
		command_line += QuoteWindowsArgument(result_directory);
		command_line += L" ";
		command_line += QuoteWindowsArgument(test_directory);

		std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
		mutable_command_line.push_back(L'\0');

		const BOOL created = CreateProcessW(
			executable_path.wstring().c_str(),
			mutable_command_line.data(),
			nullptr,
			nullptr,
			FALSE,
			CREATE_NO_WINDOW,
			nullptr,
			nullptr,
			&startup_info,
			&process_information);
		if (created == FALSE)
		{
			return std::nullopt;
		}

		ChildProcess process;
		process.m_process_information = process_information;
		process.m_started = true;
		return process;
#else
		pid_t pid = fork();
		if (pid < 0)
		{
			return std::nullopt;
		}

		if (pid == 0)
		{
			execl(
				executable_path.c_str(),
			executable_path.c_str(),
			"__test-worker",
			test_path.c_str(),
			result_directory.c_str(),
			test_directory.c_str(),
			static_cast<char*>(nullptr));
			_Exit(127);
		}

		ChildProcess process;
		process.m_pid = pid;
		process.m_started = true;
		return process;
#endif
	}

	[[nodiscard]] std::optional<int> WaitForChildProcess(ChildProcess& process, int timeout_ms)
	{
		if (!process.m_started)
		{
			return std::nullopt;
		}

#ifdef _WIN32
		const DWORD wait_result = WaitForSingleObject(process.m_process_information.hProcess, static_cast<DWORD>(timeout_ms));
		if (wait_result == WAIT_TIMEOUT)
		{
			return std::nullopt;
		}

		DWORD exit_code = EXIT_FAILURE;
		if (GetExitCodeProcess(process.m_process_information.hProcess, &exit_code) == FALSE)
		{
			return std::nullopt;
		}

		return static_cast<int>(exit_code);
#else
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		while (std::chrono::steady_clock::now() < deadline)
		{
			int status = 0;
			const pid_t wait_result = waitpid(process.m_pid, &status, WNOHANG);
			if (wait_result == process.m_pid)
			{
				if (WIFEXITED(status))
				{
					return WEXITSTATUS(status);
				}

				return EXIT_FAILURE;
			}

			if (wait_result < 0)
			{
				return std::nullopt;
			}

			usleep(1000u * 5u);
		}

		return std::nullopt;
#endif
	}

	void TerminateChildProcess(ChildProcess& process) noexcept
	{
		if (!process.m_started)
		{
			return;
		}

#ifdef _WIN32
		TerminateProcess(process.m_process_information.hProcess, EXIT_FAILURE);
		WaitForSingleObject(process.m_process_information.hProcess, 1000u);
#else
		kill(process.m_pid, SIGKILL);
		int status = 0;
		waitpid(process.m_pid, &status, 0);
#endif
	}

	void CloseChildProcess(ChildProcess& process) noexcept
	{
		if (!process.m_started)
		{
			return;
		}

#ifdef _WIN32
		CloseHandle(process.m_process_information.hThread);
		CloseHandle(process.m_process_information.hProcess);
#endif
		process.m_started = false;
	}

	// The project root as it appears in rendered diagnostics, with forward
	// slashes and a trailing slash; empty if it cannot be resolved.
	[[nodiscard]] std::string RootPrefix(const std::filesystem::path& root)
	{
		std::error_code error_code;
		const std::string resolved_root = std::filesystem::weakly_canonical(root, error_code).generic_string();
		if (resolved_root.empty())
		{
			return "";
		}
		return resolved_root + "/";
	}

	[[nodiscard]] std::string NormalizePathText(std::string text, const std::filesystem::path& root)
	{
		std::string clean = text;
		std::replace(clean.begin(), clean.end(), '\\', '/');

		const std::string root_with_slash = RootPrefix(root);
		if (!root_with_slash.empty())
		{
			size_t index = 0u;
			while ((index = clean.find(root_with_slash, index)) != std::string::npos)
			{
				clean.erase(index, root_with_slash.size());
			}
		}

		return clean;
	}

	// A snapshot that spells out this checkout's absolute path still passes here,
	// because the root is stripped from both sides before comparing, but fails in
	// any other checkout or worktree. Reject it where it is written.
	[[nodiscard]] bool EmbedsRootPath(std::string_view snapshot, const std::filesystem::path& root)
	{
		const std::string root_with_slash = RootPrefix(root);
		if (root_with_slash.empty())
		{
			return false;
		}

		std::string clean(snapshot);
		std::replace(clean.begin(), clean.end(), '\\', '/');
		return clean.find(root_with_slash) != std::string::npos;
	}

	[[nodiscard]] std::string StripAnsiCodes(std::string_view text)
	{
		std::string stripped;
		stripped.reserve(text.size());

		bool in_escape = false;
		for (const char ch : text)
		{
			if (ch == '\033')
			{
				in_escape = true;
				continue;
			}

			if (in_escape)
			{
				if (ch == 'm')
				{
					in_escape = false;
				}
				continue;
			}

			stripped.push_back(ch);
		}

		return stripped;
	}

	[[nodiscard]] std::string NormalizeSnapshotText(std::string_view text, const std::filesystem::path& root)
	{
		std::string clean = StripAnsiCodes(text);
		clean = NormalizePathText(clean, root);

		std::string normalized;
		normalized.reserve(clean.size());
		bool previous_was_carriage_return = false;
		for (const char ch : clean)
		{
			if (ch == '\r')
			{
				previous_was_carriage_return = true;
				continue;
			}

			if (previous_was_carriage_return && ch != '\n')
			{
				normalized.push_back('\n');
			}
			previous_was_carriage_return = false;
			normalized.push_back(ch);
		}
		if (previous_was_carriage_return)
		{
			normalized.push_back('\n');
		}

		std::ostringstream output;
		std::istringstream input(normalized);
		std::string line;
		bool first_line = true;
		while (std::getline(input, line))
		{
			while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
			{
				line.pop_back();
			}

			if (!first_line)
			{
				output << '\n';
			}
			output << line;
			first_line = false;
		}

		return output.str();
	}

	[[nodiscard]] std::string NormalizeJsonSnapshot(std::string_view text, const std::filesystem::path& root)
	{
		std::string normalized;
		normalized.reserve(text.size());

		bool in_string = false;
		bool escaped = false;
		for (const char ch : text)
		{
			if (in_string)
			{
				normalized.push_back(ch);
				if (escaped)
				{
					escaped = false;
				}
				else if (ch == '\\')
				{
					escaped = true;
				}
				else if (ch == '"')
				{
					in_string = false;
				}
				continue;
			}

			if (std::isspace(static_cast<unsigned char>(ch)) != 0)
			{
				continue;
			}

			normalized.push_back(ch);
			if (ch == '"')
			{
				in_string = true;
			}
		}

		return NormalizePathText(std::move(normalized), root);
	}

	[[nodiscard]] std::filesystem::path WorkerFieldPath(const std::filesystem::path& result_directory, std::string_view name)
	{
		return result_directory / std::string(name);
	}

	bool WriteWorkerResult(const MidoriTestRunner::TestResult& result, const std::filesystem::path& result_directory)
	{
		std::error_code error_code;
		std::filesystem::create_directories(result_directory, error_code);
		if (error_code)
		{
			return false;
		}

		const bool metadata_written =
			WriteTextFile(WorkerFieldPath(result_directory, "name.txt"), result.m_name) &&
			WriteTextFile(WorkerFieldPath(result_directory, "path.txt"), result.m_path.generic_string()) &&
			WriteTextFile(WorkerFieldPath(result_directory, "passed.txt"), result.m_passed ? "1" : "0") &&
			WriteTextFile(WorkerFieldPath(result_directory, "expected_to_fail.txt"), result.m_expected_to_fail ? "1" : "0") &&
			WriteTextFile(WorkerFieldPath(result_directory, "timed_out.txt"), result.m_timed_out ? "1" : "0") &&
			WriteTextFile(WorkerFieldPath(result_directory, "exit_code.txt"), std::to_string(result.m_exit_code)) &&
			WriteTextFile(WorkerFieldPath(result_directory, "duration_ms.txt"), std::to_string(static_cast<int>(result.m_duration_ms)));

		if (!metadata_written)
		{
			return false;
		}

		if (!WriteTextFile(WorkerFieldPath(result_directory, "output.txt"), result.m_output))
		{
			return false;
		}

		if (!WriteTextFile(
			WorkerFieldPath(result_directory, "error.txt"),
			result.m_error.has_value() ? std::string_view(*result.m_error) : std::string_view{}))
		{
			return false;
		}

		const std::string report_json = result.m_report_json.empty() ? result.m_report.MachineReadableJson() : result.m_report_json;
		return WriteTextFile(WorkerFieldPath(result_directory, "report.json"), report_json);
	}

	[[nodiscard]] bool ParseBoolField(const std::filesystem::path& path)
	{
		return ReadTextFile(path) == "1";
	}

	[[nodiscard]] MidoriTestRunner::TestResult ReadWorkerResult(const std::filesystem::path& result_directory)
	{
		MidoriTestRunner::TestResult result;
		result.m_name = ReadTextFile(WorkerFieldPath(result_directory, "name.txt"));
		result.m_path = ReadTextFile(WorkerFieldPath(result_directory, "path.txt"));
		result.m_passed = ParseBoolField(WorkerFieldPath(result_directory, "passed.txt"));
		result.m_expected_to_fail = ParseBoolField(WorkerFieldPath(result_directory, "expected_to_fail.txt"));
		result.m_timed_out = ParseBoolField(WorkerFieldPath(result_directory, "timed_out.txt"));
		result.m_output = ReadTextFile(WorkerFieldPath(result_directory, "output.txt"));
		result.m_report_json = ReadTextFile(WorkerFieldPath(result_directory, "report.json"));
		result.m_exit_code = std::atoi(ReadTextFile(WorkerFieldPath(result_directory, "exit_code.txt")).c_str());
		result.m_duration_ms = static_cast<double>(std::atoi(ReadTextFile(WorkerFieldPath(result_directory, "duration_ms.txt")).c_str()));

		const std::string error_text = ReadTextFile(WorkerFieldPath(result_directory, "error.txt"));
		if (!error_text.empty())
		{
			result.m_error = error_text;
		}

		return result;
	}

	[[nodiscard]] bool IsFailureTest(const std::filesystem::path& path)
	{
		for (const std::filesystem::path& part : path.parent_path())
		{
			std::string lowered = part.string();
			std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered == "failure")
			{
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] bool MatchesFilter(const std::filesystem::path& relative_path, const std::optional<std::string>& filter, const std::optional<std::string>& pattern)
	{
		if (filter.has_value())
		{
			const std::string relative_text = relative_path.generic_string();
			if (relative_text.find(*filter) == std::string::npos)
			{
				return false;
			}
		}

		if (pattern.has_value())
		{
			std::string file_name = relative_path.filename().string();
			std::string lowered_file_name = file_name;
			std::string lowered_pattern = *pattern;
			std::ranges::transform(lowered_file_name, lowered_file_name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::ranges::transform(lowered_pattern, lowered_pattern.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowered_file_name.find(lowered_pattern) == std::string::npos)
			{
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::vector<std::filesystem::path> DiscoverTests(
		const std::filesystem::path& test_directory,
		const MidoriTestRunner::Options& options)
	{
		std::vector<std::filesystem::path> tests;
		if (options.m_test_file.has_value())
		{
			const std::filesystem::path candidate = test_directory / *options.m_test_file;
			if (std::filesystem::exists(candidate))
			{
				tests.push_back(candidate);
				return tests;
			}

			const std::filesystem::path candidate_with_extension = test_directory / (*options.m_test_file + ".mmt");
			if (std::filesystem::exists(candidate_with_extension))
			{
				tests.push_back(candidate_with_extension);
				return tests;
			}
		}

		std::error_code error_code;
		if (!std::filesystem::exists(test_directory, error_code))
		{
			return tests;
		}

		for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(test_directory))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".mmt")
			{
				continue;
			}

			const std::filesystem::path relative_path = std::filesystem::relative(entry.path(), test_directory, error_code);
			if (error_code)
			{
				continue;
			}

			if (!relative_path.empty() && *relative_path.begin() == "doc_examples")
			{
				continue;
			}

			if (entry.path().parent_path() == test_directory)
			{
				const std::string file_name = entry.path().filename().string();
				if (file_name == "minimal_test.mmt" || file_name == "test.mmt" || file_name == "simple_test.mmt" || file_name == "test_backup.mmt")
				{
					continue;
				}
			}

			if (!MatchesFilter(relative_path, options.m_filter, options.m_pattern))
			{
				continue;
			}

			tests.push_back(entry.path());
		}

		std::ranges::sort(tests);
		return tests;
	}

	[[nodiscard]] std::string HumanReadableOutputForFailure(const MidoriDriver::DriverError& error)
	{
		return error.Rendered();
	}

	struct ProjectContext
	{
		std::filesystem::path m_root;
		std::filesystem::path m_test_directory;
	};

	[[nodiscard]] ProjectContext ResolveProjectContext(const std::filesystem::path& test_path)
	{
		const std::optional<MidoriProject::ManifestConfiguration> manifest = MidoriProject::FindManifestConfiguration(test_path);
		if (manifest.has_value())
		{
			return ProjectContext
			{
				.m_root = manifest->m_root,
				.m_test_directory = manifest->m_root / manifest->m_test.m_directory
			};
		}

		return ProjectContext
		{
			.m_root = test_path.parent_path(),
			.m_test_directory = test_path.parent_path()
		};
	}

	[[nodiscard]] MidoriTestRunner::TestResult MakeTimeoutResult(
		const std::filesystem::path& test_directory,
		const std::filesystem::path& test_path,
		int timeout_ms)
	{
		MidoriTestRunner::TestResult result;
		std::error_code error_code;
		const std::filesystem::path relative_path = std::filesystem::relative(test_path, test_directory, error_code);
		result.m_path = error_code ? test_path.filename() : relative_path;
		result.m_name = result.m_path.generic_string();
		result.m_expected_to_fail = IsFailureTest(result.m_path);
		result.m_timed_out = true;
		result.m_exit_code = EXIT_FAILURE;
		result.m_duration_ms = static_cast<double>(timeout_ms);
		result.m_report_json = EmptyReportJson();
		result.m_error = std::format("Timed out after {}ms.", timeout_ms);
		return result;
	}

	[[nodiscard]] MidoriTestRunner::TestResult MakeWorkerFailureResult(
		const std::filesystem::path& test_directory,
		const std::filesystem::path& test_path,
		std::string_view message)
	{
		MidoriTestRunner::TestResult result = MakeTimeoutResult(test_directory, test_path, 0);
		result.m_timed_out = false;
		result.m_error = std::string(message);
		return result;
	}

	[[nodiscard]] MidoriTestRunner::TestResult RunOneTestInProcess(
		const std::filesystem::path& root,
		const std::filesystem::path& test_directory,
		const std::filesystem::path& test_path)
	{
		const auto start = std::chrono::steady_clock::now();
		std::error_code error_code;
		const std::filesystem::path relative_path = std::filesystem::relative(test_path, test_directory, error_code);

		MidoriTestRunner::TestResult result;
		result.m_name = (error_code ? test_path.filename() : relative_path).generic_string();
		result.m_path = error_code ? test_path.filename() : relative_path;
		result.m_expected_to_fail = IsFailureTest(result.m_path);

		std::filesystem::path expected_output_path = test_path;
		expected_output_path.replace_extension(".expected");
		std::filesystem::path expected_warnings_path = test_path;
		expected_warnings_path.replace_extension(".warnings.json");

		const std::string expected_output = ReadTextFile(expected_output_path);
		const std::string expected_warnings = ReadTextFile(expected_warnings_path);

		MidoriDriver::CompileFileWithReportResult compile_result = MidoriDriver::CompileFileWithReport(test_path);
		if (!compile_result.has_value())
		{
			result.m_output = HumanReadableOutputForFailure(compile_result.error());
			if (compile_result.error().m_report.has_value())
			{
				result.m_report = *compile_result.error().m_report;
			}
			result.m_report_json = result.m_report.MachineReadableJson();
			result.m_exit_code = EXIT_FAILURE;
		}
		else
		{
			MidoriResult::CompiledProgram compiled_program = std::move(compile_result).value();
			result.m_report = compiled_program.Report();
			result.m_report_json = result.m_report.MachineReadableJson();
			result.m_output = result.m_report.RenderedWarnings();

			MidoriUtility::OutputCapture capture;
			MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(compiled_program).TakeExecutable());
			if (!run_result.has_value())
			{
				const RuntimeError runtime_error = run_result.error();
				std::print("{}", runtime_error.Rendered());
				MidoriUtility::CapturedOutput captured_output = capture.Stop();
				result.m_output += captured_output.m_stdout;
				result.m_output += captured_output.m_stderr;
				result.m_report.AppendErrors(MidoriResult::CompilerDiagnostics(runtime_error.ToCompilerError()));
				result.m_report_json = result.m_report.MachineReadableJson();
				result.m_exit_code = runtime_error.ExitCode();
			}
			else
			{
				MidoriUtility::CapturedOutput captured_output = capture.Stop();
				result.m_output += captured_output.m_stdout;
				result.m_output += captured_output.m_stderr;
				result.m_exit_code = run_result.value();
			}
		}

		const bool command_succeeded = result.m_exit_code == 0;
		result.m_passed = result.m_expected_to_fail ? !command_succeeded : command_succeeded;
		if (!result.m_passed)
		{
			result.m_error = result.m_expected_to_fail
				? std::format("Expected a non-zero exit code, got {}.", result.m_exit_code)
				: std::format("Expected exit code 0, got {}.", result.m_exit_code);
		}

		if (result.m_passed && (EmbedsRootPath(expected_output, root) || EmbedsRootPath(expected_warnings, root)))
		{
			result.m_passed = false;
			result.m_error = std::format("Snapshot contains the absolute project path '{}'. Write paths relative to the project root so the snapshot passes in other checkouts.", RootPrefix(root));
		}

		if (result.m_passed && !expected_output.empty())
		{
			const std::string normalized_expected = NormalizeSnapshotText(expected_output, root);
			const std::string normalized_actual = NormalizeSnapshotText(result.m_output, root);
			if (normalized_expected != normalized_actual)
			{
				result.m_passed = false;
				result.m_error = std::format("Output snapshot mismatch.\nExpected:\n{}\n\nActual:\n{}", normalized_expected, normalized_actual);
			}
		}

		if (result.m_passed && !expected_warnings.empty())
		{
			const std::string normalized_expected = NormalizeJsonSnapshot(expected_warnings, root);
			const std::string normalized_actual = NormalizeJsonSnapshot(result.m_report.Warnings().MachineReadableJson(), root);
			if (normalized_expected != normalized_actual)
			{
				result.m_passed = false;
				result.m_error = std::format(
					"Warning snapshot mismatch.\nExpected:\n{}\n\nActual:\n{}",
					expected_warnings,
					result.m_report.Warnings().MachineReadableJson());
			}
		}

		const auto end = std::chrono::steady_clock::now();
		result.m_duration_ms = std::chrono::duration<double, std::milli>(end - start).count();
		return result;
	}

	[[nodiscard]] MidoriTestRunner::TestResult RunOneTest(
		const std::filesystem::path& executable_path,
		const std::filesystem::path& test_directory,
		const std::filesystem::path& test_path,
		int timeout_ms)
	{
		const std::filesystem::path result_directory = MakeWorkerResultDirectory();
		if (result_directory.empty())
		{
			return MakeWorkerFailureResult(test_directory, test_path, "Failed to allocate a worker result directory.");
		}

		std::optional<ChildProcess> process = StartWorkerProcess(executable_path, test_path, result_directory, test_directory);
		if (!process.has_value())
		{
			std::error_code cleanup_error;
			std::filesystem::remove_all(result_directory, cleanup_error);
			return MakeWorkerFailureResult(test_directory, test_path, "Failed to start the Marmot test worker process.");
		}

		const std::optional<int> exit_code = WaitForChildProcess(*process, timeout_ms);
		if (!exit_code.has_value())
		{
			TerminateChildProcess(*process);
			CloseChildProcess(*process);
			std::error_code cleanup_error;
			std::filesystem::remove_all(result_directory, cleanup_error);
			return MakeTimeoutResult(test_directory, test_path, timeout_ms);
		}

		CloseChildProcess(*process);

		MidoriTestRunner::TestResult result = ReadWorkerResult(result_directory);
		std::error_code cleanup_error;
		std::filesystem::remove_all(result_directory, cleanup_error);
		if (result.m_name.empty())
		{
			return MakeWorkerFailureResult(test_directory, test_path, "The Marmot test worker did not return a result.");
		}

		return result;
	}

	[[nodiscard]] std::string StatusLabel(const MidoriTestRunner::TestResult& result)
	{
		if (result.m_timed_out)
		{
			return std::format("{}[TIMEOUT]{}", Yellow, Reset);
		}

		return result.m_passed
			? std::format("{}[OK]{}", Green, Reset)
			: std::format("{}[FAIL]{}", Red, Reset);
	}

	[[nodiscard]] std::string TestTypeLabel(const MidoriTestRunner::TestResult& result)
	{
		return result.m_expected_to_fail
			? std::format("{}[SHOULD-FAIL]{}", Yellow, Reset)
			: std::format("{}[SUCCESS]{}", Cyan, Reset);
	}
}

namespace MidoriTestRunner
{
	int RunResult::TotalCount() const
	{
		return static_cast<int>(m_results.size());
	}

	int RunResult::PassedCount() const
	{
		return static_cast<int>(std::ranges::count_if(m_results, [](const TestResult& result) { return result.m_passed; }));
	}

	int RunResult::FailedCount() const
	{
		return TotalCount() - PassedCount();
	}

	double RunResult::TotalDurationMs() const
	{
		double total = 0.0;
		for (const TestResult& result : m_results)
		{
			total += result.m_duration_ms;
		}
		return total;
	}

	bool RunResult::Succeeded() const
	{
		return FailedCount() == 0;
	}

	std::string RunResult::Rendered() const
	{
		std::ostringstream output;
		output << Bold << "Marmot Test Suite" << Reset << "\n";
		output << Gray << "============================================================" << Reset << "\n";
		output << "Root: " << Cyan << m_root.string() << Reset << "\n";
		output << "Tests: " << Cyan << TotalCount() << Reset << "\n";
		output << "Timeout: " << Cyan << m_timeout_ms << "ms" << Reset << "\n";
		output << Gray << "============================================================" << Reset << "\n";

		std::string last_category;
		for (const TestResult& result : m_results)
		{
			const std::filesystem::path path = result.m_path;
			const std::string category = path.begin() != path.end() ? (*path.begin()).string() : std::string("tests");
			if (category != last_category)
			{
				output << "\n" << Bold << Blue << "[" << category << "]" << Reset << "\n";
				last_category = category;
			}

			output << StatusLabel(result) << " " << TestTypeLabel(result) << " " << result.m_name
				<< " " << Gray << "(" << std::format("{:.0f}", result.m_duration_ms) << "ms)" << Reset << "\n";
			if (!result.m_passed && result.m_error.has_value())
			{
				output << "  " << Red << *result.m_error << Reset << "\n";
			}
		}

		output << "\n" << Gray << "============================================================" << Reset << "\n";
		output << Bold << "Test Summary" << Reset << "\n";
		output << (Succeeded()
			? std::format("{}{}[SUCCESS] All tests passed!{}\n", Green, Bold, Reset)
			: std::format("{}{}[FAILED] Some tests failed{}\n", Red, Bold, Reset));
		const int timed_out_count = static_cast<int>(std::ranges::count_if(m_results, [](const TestResult& result) { return result.m_timed_out; }));
		output << "Total: " << PassedCount() << "/" << TotalCount() << " passed\n";
		if (timed_out_count > 0)
		{
			output << "Timed out: " << timed_out_count << "\n";
		}
		output << "Duration: " << std::format("{:.0f}", TotalDurationMs()) << "ms\n";
		return output.str();
	}

	std::string RunResult::MachineReadableJson() const
	{
		std::string results_json = "[";
		for (size_t index = 0u; index < m_results.size(); index += 1u)
		{
			if (index > 0u)
			{
				results_json.push_back(',');
			}

			const TestResult& result = m_results[index];
			std::string object = "{";
			bool first_field = true;
			MidoriJson::AppendStringField(object, "name", result.m_name, first_field);
			MidoriJson::AppendStringField(object, "path", result.m_path.generic_string(), first_field);
			MidoriJson::AppendBoolField(object, "passed", result.m_passed, first_field);
			MidoriJson::AppendBoolField(object, "expectedToFail", result.m_expected_to_fail, first_field);
			MidoriJson::AppendBoolField(object, "timedOut", result.m_timed_out, first_field);
			MidoriJson::AppendNumberField(object, "exitCode", result.m_exit_code, first_field);
			MidoriJson::AppendNumberField(object, "durationMs", static_cast<int>(result.m_duration_ms), first_field);
			MidoriJson::AppendStringField(
				object,
				"error",
				result.m_error.has_value() ? std::optional<std::string_view>(*result.m_error) : std::nullopt,
				first_field);
			MidoriJson::AppendStringField(object, "output", result.m_output, first_field);
			MidoriJson::AppendRawField(
				object,
				"report",
				result.m_report_json.empty() ? result.m_report.MachineReadableJson() : result.m_report_json,
				first_field);
			object.push_back('}');
			results_json += object;
		}
		results_json.push_back(']');

		std::string summary = "{";
		bool summary_first_field = true;
		MidoriJson::AppendNumberField(summary, "total", TotalCount(), summary_first_field);
		MidoriJson::AppendNumberField(summary, "passed", PassedCount(), summary_first_field);
		MidoriJson::AppendNumberField(summary, "failed", FailedCount(), summary_first_field);
		MidoriJson::AppendNumberField(
			summary,
			"timedOut",
			static_cast<int>(std::ranges::count_if(m_results, [](const TestResult& result) { return result.m_timed_out; })),
			summary_first_field);
		MidoriJson::AppendNumberField(summary, "durationMs", static_cast<int>(TotalDurationMs()), summary_first_field);
		summary.push_back('}');

		std::string payload = "{";
		bool first_field = true;
		MidoriJson::AppendNumberField(payload, "version", 1, first_field);
		MidoriJson::AppendStringField(payload, "source", "marmot", first_field);
		MidoriJson::AppendStringField(payload, "command", "test", first_field);
		MidoriJson::AppendBoolField(payload, "success", Succeeded(), first_field);
		MidoriJson::AppendStringField(payload, "root", m_root.generic_string(), first_field);
		MidoriJson::AppendStringField(payload, "testDir", m_test_directory.generic_string(), first_field);
		MidoriJson::AppendRawField(payload, "summary", summary, first_field);
		MidoriJson::AppendRawField(payload, "results", results_json, first_field);
		payload.push_back('}');
		return payload;
	}

	RunResult Run(const Options& options)
	{
		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		RunResult run_result;

		const std::filesystem::path resolved_start_path =
			options.m_start_path.empty() ? std::filesystem::current_path() : options.m_start_path;
		const std::optional<MidoriProject::ManifestConfiguration> manifest = MidoriProject::FindManifestConfiguration(resolved_start_path);
		run_result.m_root = manifest.has_value() ? manifest->m_root : resolved_start_path;
		run_result.m_timeout_ms = manifest.has_value() ? manifest->m_test.m_timeout_ms : 30000;
		run_result.m_test_directory = run_result.m_root / (manifest.has_value() ? manifest->m_test.m_directory : std::filesystem::path("test"));
		const std::filesystem::path executable_path = CurrentExecutablePath();

		const std::vector<std::filesystem::path> tests = DiscoverTests(run_result.m_test_directory, options);
		for (const std::filesystem::path& test_path : tests)
		{
			if (executable_path.empty())
			{
				run_result.m_results.push_back(MakeWorkerFailureResult(run_result.m_test_directory, test_path, "Failed to resolve the current Marmot executable path."));
				continue;
			}

			run_result.m_results.push_back(RunOneTest(executable_path, run_result.m_test_directory, test_path, run_result.m_timeout_ms));
		}

		return run_result;
	}

	int RunWorker(const WorkerOptions& options)
	{
		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		const std::filesystem::path absolute_test_path = std::filesystem::absolute(options.m_test_path);
		ProjectContext project_context = ResolveProjectContext(absolute_test_path);
		if (!options.m_test_directory.empty())
		{
			project_context.m_test_directory = std::filesystem::absolute(options.m_test_directory);
			project_context.m_root = project_context.m_test_directory.parent_path();
		}
		MidoriTestRunner::TestResult result = RunOneTestInProcess(project_context.m_root, project_context.m_test_directory, absolute_test_path);
		if (!WriteWorkerResult(result, options.m_result_directory))
		{
			return EXIT_FAILURE;
		}

		return result.m_passed ? EXIT_SUCCESS : EXIT_FAILURE;
	}
}
