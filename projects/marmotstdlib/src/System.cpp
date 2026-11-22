#include "MarmotStdLibExports.h"
#include "FFIAllocation/FFIAllocation.h"
#include "TextPath.h"
#include "ProcessState.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <string>
#include <system_error>
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

#ifndef __EMSCRIPTEN__
#include <reproc/run.h>
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
#else
	std::expected<ProcessOutcome, std::string> RunToCompletion(const std::vector<std::string>& arguments, const std::string& directory)
	{
		std::vector<const char*> argv;
		argv.reserve(arguments.size() + 1u);
		for (const std::string& argument : arguments)
		{
			argv.push_back(argument.c_str());
		}
		argv.push_back(nullptr);

		reproc_options options{};
		options.working_directory = directory.empty() ? nullptr : directory.c_str();
		options.redirect.in.type = REPROC_REDIRECT_DISCARD;
		options.redirect.out.type = REPROC_REDIRECT_PIPE;
		options.redirect.err.type = REPROC_REDIRECT_PIPE;

		const auto append = [](REPROC_STREAM, const uint8_t* bytes, size_t size, void* target) -> int
		{
			if (size != 0u)
			{
				static_cast<std::string*>(target)->append(reinterpret_cast<const char*>(bytes), size);
			}
			return 0;
		};

		ProcessOutcome outcome;
		const int status = reproc_run_ex(argv.data(), options, { append, &outcome.m_output }, { append, &outcome.m_error });
		if (status < 0)
		{
			return std::unexpected(std::string(reproc_strerror(status)));
		}

		outcome.m_exit_code = status;
		return outcome;
	}
#endif

	MarmotFFIArray* RunProcess(
		const std::vector<std::string>& arguments,
		std::string_view directory
	) noexcept
	{
		std::error_code ec;
		if (!directory.empty() && !std::filesystem::is_directory(TextPath::FromText(directory), ec))
		{
			SetLastSystemError(SystemErrorKind::InvalidDirectory, "Not a directory.");
			return nullptr;
		}

		const std::expected<ProcessOutcome, std::string> outcome = RunToCompletion(arguments, std::string(directory));
		if (outcome.has_value())
		{
			ClearLastSystemError();
		}
		else
		{
			SetLastSystemError(SystemErrorKind::Unknown, outcome.error());
			return nullptr;
		}

		return FFIAllocation::AllocateStringArray(
			{ std::to_string(outcome->m_exit_code), outcome->m_output, outcome->m_error });
	}
}

extern "C"
{
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(RunProcess)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const MarmotFFIArray& array = *static_cast<const MarmotFFIArray*>(args[0u]);
		const char* const* data = static_cast<const char* const*>(array.data);
		std::vector<std::string> arguments;
		arguments.reserve(static_cast<size_t>(array.length));
		for (int32_t index = 0; index < array.length; index += 1)
		{
			arguments.emplace_back(data[index]);
		}
		MarmotFFIArray* result = RunProcess(arguments, static_cast<const char*>(args[1u]));
		std::memcpy(ret, &result, sizeof(result));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetEnv)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
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


	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetCurrentDirectory)(void**, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		std::error_code ec;
		const std::filesystem::path cwd = std::filesystem::current_path(ec);
		SetLastSystemErrorFromDirectoryCode(ec);

		char* result = ec ? AllocateString("") : AllocateString(TextPath::ToText(cwd));
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(SetCurrentDirectory)(void** args, void* ret) noexcept
	{
		const std::unique_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		std::filesystem::current_path(TextPath::FromText(path), ec);
		SetLastSystemErrorFromDirectoryCode(ec);

		const int64_t success = ec ? 0 : 1;
		std::memcpy(ret, &success, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(SetEnv)(void** args, void* ret) noexcept
	{
		const std::unique_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
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

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Execute)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
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



	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetPlatform)(void**, void* ret) noexcept
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

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetProcessId)(void**, void* ret) noexcept
	{
#ifdef _WIN32
		const int64_t pid = static_cast<int64_t>(GetCurrentProcessId());
#else
		const int64_t pid = static_cast<int64_t>(getpid());
#endif
		std::memcpy(ret, &pid, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetLastSystemErrorKind)(void**, void* ret) noexcept
	{
		const int64_t result = static_cast<int64_t>(s_last_system_error.m_kind);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetLastSystemErrorMessage)(void**, void* ret) noexcept
	{
		char* result = AllocateString(s_last_system_error.m_message);
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}
}
