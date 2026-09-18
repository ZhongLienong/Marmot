#include "Common/Cancellation/Cancellation.h"
#include "Library/MidoriStdLibExports.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
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

	thread_local SystemErrorState s_last_system_error{};

	char* AllocateString(const std::string& str)
	{
		const size_t size = str.size() + 1u;
		char* buffer = static_cast<char*>(std::malloc(size));
		if (buffer != nullptr)
		{
			std::memcpy(buffer, str.c_str(), size);
		}
		return buffer;
	}

	char* AllocateEmptyString()
	{
		char* buffer = static_cast<char*>(std::malloc(1u));
		if (buffer != nullptr)
		{
			buffer[0u] = '\0';
		}
		return buffer;
	}

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
		const DWORD required_size = GetEnvironmentVariableA(var_name, nullptr, 0u);
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

			char* empty = AllocateEmptyString();
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(int64_t));
			return;
		}

		std::string value(static_cast<size_t>(required_size) - 1u, '\0');
		const DWORD written = GetEnvironmentVariableA(var_name, value.data(), required_size);
		if (written + 1u != required_size)
		{
			const DWORD error = GetLastError();
			SetLastSystemError(SystemErrorKind::Unknown, std::system_category().message(static_cast<int>(error)));
			char* empty = AllocateEmptyString();
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(int64_t));
			return;
		}

		ClearLastSystemError();
		char* result = AllocateString(value);
#else
		const char* value = std::getenv(var_name);
		if (value == nullptr)
		{
			SetLastSystemError(SystemErrorKind::MissingEnv, std::string(var_name));
			char* empty = AllocateEmptyString();
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

		char* result = ec ? AllocateEmptyString() : AllocateString(cwd.string());
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SetCurrentDirectory)(void** args, void* ret) noexcept
	{
		const char* path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		std::filesystem::current_path(path, ec);
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
		const BOOL set_result = SetEnvironmentVariableA(name, value);
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
		const int exit_code = std::system(command);
		if (exit_code == -1)
		{
			SetLastSystemError(SystemErrorKind::Unknown, "Failed to execute command.");
		}
		else
		{
			ClearLastSystemError();
		}

		const int64_t result = static_cast<int64_t>(exit_code);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetPlatform)(void**, void* ret) noexcept
	{
#if defined(_WIN32)
		char* result = AllocateString("Windows");
#elif defined(__APPLE__)
		char* result = AllocateString("macOS");
#elif defined(__linux__)
		char* result = AllocateString("Linux");
#elif defined(MIDORI_WASM64)
		char* result = AllocateString("WebAssembly (wasm64)");
#elif defined(__EMSCRIPTEN__) || defined(MIDORI_WASM)
		char* result = AllocateString("WebAssembly");
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
		char* result = s_last_system_error.m_message.empty()
			? AllocateEmptyString()
			: AllocateString(s_last_system_error.m_message);
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}
}
