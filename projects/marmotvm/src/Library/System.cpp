#include "Support/Cancellation/Cancellation.h"
#include "Library/MidoriStdLibExports.h"
#include "Library/FFIAllocation/FFIAllocation.h"
#include "Library/TextPath/TextPath.h"
#include "Value/Value.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <spawn.h>

extern char** environ;
#endif

namespace
{
	enum class SystemErrorKind : int64_t
	{
		Success = 0,
		MissingEnv = 1,
		InvalidDirectory = 2,
		Unknown = 3
	};

	struct SystemErrorState
	{
		SystemErrorKind m_kind = SystemErrorKind::Success;
		std::string m_message;
	};

	using FFIAllocation::AllocateString;

	thread_local SystemErrorState s_last_system_error{};

	void ClearLastSystemError()
	{
		s_last_system_error.m_kind = SystemErrorKind::Success;
		s_last_system_error.m_message.clear();
	}

	void SetLastSystemError(SystemErrorKind kind, const std::string& message)
	{
		s_last_system_error.m_kind = kind;
		s_last_system_error.m_message = message;
	}

	void SetLastSystemErrorFromDirectoryCode(const std::error_code& ec)
	{
		if (!ec)
		{
			ClearLastSystemError();
			return;
		}

		SetLastSystemError(SystemErrorKind::InvalidDirectory, ec.message());
	}

#ifdef _WIN32
	// Text is UTF-8, and the wide Windows calls take it as UTF-16; the narrow
	// ones would read it in the code page.
	std::wstring Widen(std::string_view text)
	{
		const int length = MultiByteToWideChar(CP_UTF8, 0u, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring wide(static_cast<size_t>(length), L'\0');
		MultiByteToWideChar(CP_UTF8, 0u, text.data(), static_cast<int>(text.size()), wide.data(), length);
		return wide;
	}

	std::string Narrow(std::wstring_view text)
	{
		const int length = WideCharToMultiByte(CP_UTF8, 0u, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
		std::string narrow(static_cast<size_t>(length), '\0');
		WideCharToMultiByte(CP_UTF8, 0u, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr, nullptr);
		return narrow;
	}
#endif

	struct ProcessOutcome
	{
		int m_exit_code = 0;
		std::string m_output;
		std::string m_error;
	};

#if defined(__EMSCRIPTEN__)
	std::expected<ProcessOutcome, std::string> RunToCompletion(const std::vector<std::string>&, const std::string&)
	{
		return std::unexpected(std::string("WebAssembly cannot start a process."));
	}
#elif defined(_WIN32)
	// Quoted the way the C runtime splits a command line back into arguments:
	// backslashes are literal except in a run that ends at a quote.
	void AppendQuoted(std::wstring& command_line, std::wstring_view argument)
	{
		if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
		{
			command_line.append(argument);
			return;
		}

		command_line.push_back(L'"');
		size_t backslashes = 0u;
		for (const wchar_t character : argument)
		{
			if (character == L'\\')
			{
				backslashes += 1u;
				continue;
			}

			command_line.append(character == L'"' ? (backslashes * 2u) + 1u : backslashes, L'\\');
			command_line.push_back(character);
			backslashes = 0u;
		}
		command_line.append(backslashes * 2u, L'\\');
		command_line.push_back(L'"');
	}

	void CloseIfOpen(std::initializer_list<HANDLE> handles)
	{
		for (const HANDLE handle : handles)
		{
			if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
			{
				CloseHandle(handle);
			}
		}
	}

	std::string ReadAll(HANDLE pipe)
	{
		std::string text;
		std::array<char, 4096u> buffer{};
		DWORD count = 0u;
		while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count > 0u)
		{
			text.append(buffer.data(), count);
		}
		CloseHandle(pipe);
		return text;
	}

	std::expected<ProcessOutcome, std::string> RunToCompletion(const std::vector<std::string>& arguments, const std::string& directory)
	{
		SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE output_read = nullptr;
		HANDLE output_write = nullptr;
		HANDLE error_read = nullptr;
		HANDLE error_write = nullptr;
		const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0u, nullptr);
		if (input == INVALID_HANDLE_VALUE
			|| !CreatePipe(&output_read, &output_write, &inheritable, 0u)
			|| !CreatePipe(&error_read, &error_write, &inheritable, 0u))
		{
			const DWORD error = GetLastError();
			CloseIfOpen({ input, output_read, output_write, error_read, error_write });
			return std::unexpected(std::system_category().message(static_cast<int>(error)));
		}

		// Only the child's ends cross, so a process another worker starts at
		// the same moment cannot hold this one's pipes open.
		std::array<HANDLE, 3u> inherited{ input, output_write, error_write };
		SIZE_T attributes_size = 0u;
		InitializeProcThreadAttributeList(nullptr, 1u, 0u, &attributes_size);
		std::vector<std::byte> attributes_storage(attributes_size);
		const LPPROC_THREAD_ATTRIBUTE_LIST attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes_storage.data());
		InitializeProcThreadAttributeList(attributes, 1u, 0u, &attributes_size);
		UpdateProcThreadAttribute(attributes, 0u, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), sizeof(inherited), nullptr, nullptr);

		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = input;
		startup.StartupInfo.hStdOutput = output_write;
		startup.StartupInfo.hStdError = error_write;
		startup.lpAttributeList = attributes;

		std::wstring command_line;
		for (const std::string& argument : arguments)
		{
			if (!command_line.empty())
			{
				command_line.push_back(L' ');
			}
			AppendQuoted(command_line, Widen(argument));
		}

		const std::wstring wide_directory = Widen(directory);
		PROCESS_INFORMATION process{};
		const BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, wide_directory.empty() ? nullptr : wide_directory.c_str(), &startup.StartupInfo, &process);
		const DWORD create_error = GetLastError();
		DeleteProcThreadAttributeList(attributes);
		CloseIfOpen({ input, output_write, error_write });
		if (!created)
		{
			CloseIfOpen({ output_read, error_read });
			return std::unexpected(std::system_category().message(static_cast<int>(create_error)));
		}
		CloseHandle(process.hThread);

		ProcessOutcome outcome;
		{
			std::jthread error_reader([&outcome, error_read]() { outcome.m_error = ReadAll(error_read); });
			outcome.m_output = ReadAll(output_read);
		}

		WaitForSingleObject(process.hProcess, INFINITE);
		DWORD exit_code = 0u;
		GetExitCodeProcess(process.hProcess, &exit_code);
		CloseHandle(process.hProcess);
		outcome.m_exit_code = static_cast<int>(exit_code);
		return outcome;
	}
#else
	// Close-on-exec from the start, so a process another worker starts at the
	// same moment cannot inherit this one's pipe and hold it open.
	bool MakePipe(std::array<int, 2u>& ends)
	{
#ifdef __linux__
		return pipe2(ends.data(), O_CLOEXEC) == 0;
#else
		return pipe(ends.data()) == 0
			&& fcntl(ends[0u], F_SETFD, FD_CLOEXEC) == 0
			&& fcntl(ends[1u], F_SETFD, FD_CLOEXEC) == 0;
#endif
	}

	std::string ReadAll(int descriptor)
	{
		std::string text;
		std::array<char, 4096u> buffer{};
		for (;;)
		{
			const ssize_t count = read(descriptor, buffer.data(), buffer.size());
			if (count > 0)
			{
				text.append(buffer.data(), static_cast<size_t>(count));
			}
			else if (count == 0 || errno != EINTR)
			{
				break;
			}
		}
		close(descriptor);
		return text;
	}

	std::expected<ProcessOutcome, std::string> RunToCompletion(const std::vector<std::string>& arguments, const std::string& directory)
	{
		std::array<int, 2u> output{ -1, -1 };
		std::array<int, 2u> error{ -1, -1 };
		if (!MakePipe(output) || !MakePipe(error))
		{
			const int pipe_error = errno;
			for (const int descriptor : { output[0u], output[1u], error[0u], error[1u] })
			{
				if (descriptor != -1)
				{
					close(descriptor);
				}
			}
			return std::unexpected(std::generic_category().message(pipe_error));
		}

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		posix_spawn_file_actions_adddup2(&actions, output[1u], STDOUT_FILENO);
		posix_spawn_file_actions_adddup2(&actions, error[1u], STDERR_FILENO);
		if (!directory.empty())
		{
			posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
		}

		std::vector<char*> argv = arguments
			| std::views::transform([](const std::string& argument) { return const_cast<char*>(argument.c_str()); })
			| std::ranges::to<std::vector>();
		argv.push_back(nullptr);

		pid_t process = 0;
		const int spawned = posix_spawnp(&process, argv.front(), &actions, nullptr, argv.data(), environ);
		posix_spawn_file_actions_destroy(&actions);
		close(output[1u]);
		close(error[1u]);
		if (spawned != 0)
		{
			close(output[0u]);
			close(error[0u]);
			return std::unexpected(std::generic_category().message(spawned));
		}

		ProcessOutcome outcome;
		{
			std::jthread error_reader([&outcome, descriptor = error[0u]]() { outcome.m_error = ReadAll(descriptor); });
			outcome.m_output = ReadAll(output[0u]);
		}

		int status = 0;
		while (waitpid(process, &status, 0) == -1 && errno == EINTR)
		{
		}
		// A program killed by a signal reports 128 + the signal, as a shell does.
		outcome.m_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		return outcome;
	}
#endif
}

extern "C"
{
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Exit)(void** args, void*) noexcept
	{
		int64_t exit_code = 0;
		std::memcpy(&exit_code, &args[0u], sizeof(int64_t));
		std::exit(static_cast<int>(exit_code));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetEnv)(void** args, void* ret) noexcept
	{
		const char* var_name = reinterpret_cast<const char*>(args[0u]);

#ifdef _WIN32
		const std::wstring wide_name = Widen(var_name);
		const DWORD required_size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0u);
		if (required_size == 0u)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_ENVVAR_NOT_FOUND)
			{
				SetLastSystemError(SystemErrorKind::MissingEnv, std::string(var_name));
			}
			else
			{
				SetLastSystemError(SystemErrorKind::Unknown, std::system_category().message(static_cast<int>(error)));
			}

			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(int64_t));
			return;
		}

		std::wstring value(static_cast<size_t>(required_size) - 1u, L'\0');
		const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), value.data(), required_size);
		if (written + 1u != required_size)
		{
			const DWORD error = GetLastError();
			SetLastSystemError(SystemErrorKind::Unknown, std::system_category().message(static_cast<int>(error)));
			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(int64_t));
			return;
		}

		ClearLastSystemError();
		char* result = AllocateString(Narrow(value));
#else
		const char* value = std::getenv(var_name);
		if (value == nullptr)
		{
			SetLastSystemError(SystemErrorKind::MissingEnv, std::string(var_name));
			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(int64_t));
			return;
		}

		ClearLastSystemError();
		char* result = AllocateString(value);
#endif

		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Sleep)(void** args, void* ret) noexcept
	{
		int64_t milliseconds = 0;
		std::memcpy(&milliseconds, &args[0u], sizeof(int64_t));
		ThreadCancellation::SleepInterruptible(std::chrono::milliseconds(milliseconds));
		std::memset(ret, 0, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetCurrentDirectory)(void**, void* ret) noexcept
	{
		std::error_code ec;
		const std::filesystem::path cwd = std::filesystem::current_path(ec);
		SetLastSystemErrorFromDirectoryCode(ec);

		char* result = ec ? AllocateString("") : AllocateString(TextPath::ToText(cwd));
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SetCurrentDirectory)(void** args, void* ret) noexcept
	{
		const char* path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		std::filesystem::current_path(TextPath::FromText(path), ec);
		SetLastSystemErrorFromDirectoryCode(ec);

		const int64_t success = ec ? 0 : 1;
		std::memcpy(ret, &success, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SetEnv)(void** args, void* ret) noexcept
	{
		const char* name = reinterpret_cast<const char*>(args[0u]);
		const char* value = reinterpret_cast<const char*>(args[1u]);
		int result = 0;

#ifdef _WIN32
		const BOOL set_result = SetEnvironmentVariableW(Widen(name).c_str(), Widen(value).c_str());
		result = set_result != 0 ? 1 : 0;
		if (result != 0)
		{
			ClearLastSystemError();
		}
		else
		{
			const DWORD error = GetLastError();
			SetLastSystemError(SystemErrorKind::Unknown, std::system_category().message(static_cast<int>(error)));
		}
#else
		const int set_result = setenv(name, value, 1);
		result = set_result == 0 ? 1 : 0;
		if (result != 0)
		{
			ClearLastSystemError();
		}
		else
		{
			SetLastSystemError(SystemErrorKind::Unknown, std::generic_category().message(errno));
		}
#endif

		const int64_t success = result;
		std::memcpy(ret, &success, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Execute)(void** args, void* ret) noexcept
	{
		const char* command = reinterpret_cast<const char*>(args[0u]);
#ifdef _WIN32
		const int status = _wsystem(Widen(command).c_str());
#else
		const int status = std::system(command);
#endif
		if (status == -1)
		{
			SetLastSystemError(SystemErrorKind::Unknown, "Failed to execute command.");
		}
		else
		{
			ClearLastSystemError();
		}

#ifdef _WIN32
		const int exit_code = status;
#else
		// std::system returns a wait status here, not the exit code: `exit 3` is 768.
		// A command killed by a signal reports 128 + the signal, as a shell does.
		const int exit_code = (status == -1) ? -1 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
		const int64_t result = static_cast<int64_t>(exit_code);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	// The arguments reach the program exactly, with no shell between. Both
	// outputs are read while it runs, so neither pipe fills and stops it. The
	// result is the exit code, the output and the error output, as text.
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(RunProcess)(void** args, void* ret) noexcept
	{
		const MidoriArray& values = static_cast<MidoriTraceable*>(args[0u])->GetTraceable<MidoriArray>();
		const std::vector<std::string> arguments = std::views::iota(0, values.GetLength())
			| std::views::transform([&values](int index) { return std::string(values[index].GetPointer()->GetTraceable<MidoriText>().View()); })
			| std::ranges::to<std::vector>();
		const std::string directory(reinterpret_cast<const char*>(args[1u]));

		std::error_code ec;
		if (!directory.empty() && !std::filesystem::is_directory(TextPath::FromText(directory), ec))
		{
			SetLastSystemError(SystemErrorKind::InvalidDirectory, "Not a directory.");
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		const std::expected<ProcessOutcome, std::string> outcome = RunToCompletion(arguments, directory);
		if (outcome.has_value())
		{
			ClearLastSystemError();
		}
		else
		{
			SetLastSystemError(SystemErrorKind::Unknown, outcome.error());
		}

		const int64_t ptr = outcome.has_value()
			? reinterpret_cast<int64_t>(FFIAllocation::AllocateStringArray({ std::to_string(outcome->m_exit_code), outcome->m_output, outcome->m_error }))
			: 0;
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetPlatform)(void**, void* ret) noexcept
	{
#if defined(_WIN32)
		char* result = AllocateString("Windows");
#elif defined(__APPLE__)
		char* result = AllocateString("macOS");
#elif defined(__linux__)
		char* result = AllocateString("Linux");
#elif defined(__EMSCRIPTEN__)
		char* result = AllocateString("WebAssembly (wasm64)");
#else
		char* result = AllocateString("Unknown");
#endif
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetProcessId)(void**, void* ret) noexcept
	{
#ifdef _WIN32
		const int64_t pid = static_cast<int64_t>(GetCurrentProcessId());
#else
		const int64_t pid = static_cast<int64_t>(getpid());
#endif
		std::memcpy(ret, &pid, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastSystemErrorKind)(void**, void* ret) noexcept
	{
		const int64_t result = static_cast<int64_t>(s_last_system_error.m_kind);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastSystemErrorMessage)(void**, void* ret) noexcept
	{
		char* result = AllocateString(s_last_system_error.m_message);
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}
}
