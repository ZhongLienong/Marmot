#include "Library/MidoriStdLibExports.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
	enum class IOErrorKind : int64_t
	{
		Success = 0,
		NotFound = 1,
		PermissionDenied = 2,
		InvalidPath = 3,
		Unknown = 4
	};

	struct IOErrorState
	{
		IOErrorKind m_kind = IOErrorKind::Success;
		std::string m_message;
	};

	thread_local IOErrorState s_last_io_error{};

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

	void ClearLastIOError()
	{
		s_last_io_error.m_kind = IOErrorKind::Success;
		s_last_io_error.m_message.clear();
	}

	void SetLastIOError(IOErrorKind kind, const std::string& message)
	{
		s_last_io_error.m_kind = kind;
		s_last_io_error.m_message = message;
	}

	IOErrorKind ClassifyIOError(const std::error_code& ec)
	{
		if (!ec)
		{
			return IOErrorKind::Success;
		}

		if (ec == std::errc::no_such_file_or_directory)
		{
			return IOErrorKind::NotFound;
		}

		if (ec == std::errc::permission_denied
			|| ec == std::errc::operation_not_permitted
			|| ec == std::errc::read_only_file_system)
		{
			return IOErrorKind::PermissionDenied;
		}

		if (ec == std::errc::invalid_argument
			|| ec == std::errc::filename_too_long
			|| ec == std::errc::not_a_directory
			|| ec == std::errc::is_a_directory
			|| ec == std::errc::too_many_symbolic_link_levels)
		{
			return IOErrorKind::InvalidPath;
		}

		return IOErrorKind::Unknown;
	}

	void SetLastIOErrorFromCode(const std::error_code& ec)
	{
		if (!ec)
		{
			ClearLastIOError();
			return;
		}

		SetLastIOError(ClassifyIOError(ec), ec.message());
	}

	std::error_code DiagnoseReadOpenFailure(const std::filesystem::path& path)
	{
		std::error_code ec;
		const bool exists = std::filesystem::exists(path, ec);
		if (ec)
		{
			return ec;
		}

		if (!exists)
		{
			return std::make_error_code(std::errc::no_such_file_or_directory);
		}

		const std::filesystem::file_status status = std::filesystem::status(path, ec);
		if (ec)
		{
			return ec;
		}

		if (std::filesystem::is_directory(status))
		{
			return std::make_error_code(std::errc::is_a_directory);
		}

		return std::make_error_code(std::errc::permission_denied);
	}

	std::error_code DiagnoseWriteOpenFailure(const std::filesystem::path& path)
	{
		const std::filesystem::path parent = path.parent_path();
		if (!parent.empty())
		{
			std::error_code ec;
			const bool parent_exists = std::filesystem::exists(parent, ec);
			if (ec)
			{
				return ec;
			}

			if (!parent_exists)
			{
				return std::make_error_code(std::errc::no_such_file_or_directory);
			}

			const std::filesystem::file_status parent_status = std::filesystem::status(parent, ec);
			if (ec)
			{
				return ec;
			}

			if (!std::filesystem::is_directory(parent_status))
			{
				return std::make_error_code(std::errc::not_a_directory);
			}
		}

		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::status(path, ec);
		if (ec)
		{
			return ec;
		}

		if (std::filesystem::is_directory(status))
		{
			return std::make_error_code(std::errc::is_a_directory);
		}

		return std::make_error_code(std::errc::permission_denied);
	}
}

extern "C"
{
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Print)(void** args, void* ret) noexcept
	{
		const char* str = reinterpret_cast<const char*>(args[0u]);

		#ifdef __EMSCRIPTEN__
		std::cout << str;
		std::cout.flush();
		#else
		std::printf("%s", str);
		std::fflush(stdout);
		#endif

		std::memset(ret, 0, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(PrintError)(void** args, void* ret) noexcept
	{
		const char* str = reinterpret_cast<const char*>(args[0u]);

		#ifdef __EMSCRIPTEN__
		std::cerr << str;
		std::cerr.flush();
		#else
		std::fprintf(stderr, "%s", str);
		std::fflush(stderr);
		#endif

		std::memset(ret, 0, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadInput)(void**, void* ret) noexcept
	{
		std::ostringstream buffer;
		buffer << std::cin.rdbuf();
		char* result = AllocateString(buffer.str());

		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadLine)(void**, void* ret) noexcept
	{
		std::string line;
		if (std::getline(std::cin, line))
		{
			char* result = AllocateString(line);
			const int64_t ptr = reinterpret_cast<int64_t>(result);
			std::memcpy(ret, &ptr, sizeof(double));
		}
		else
		{
			char* empty = AllocateEmptyString();
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
		}
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadFile)(void** args, void* ret) noexcept
	{
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		const std::filesystem::path path(file_path);

		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseReadOpenFailure(path));
			char* empty = AllocateEmptyString();
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
			return;
		}

		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (file.bad())
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to read file.");
			char* empty = AllocateEmptyString();
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
			return;
		}

		ClearLastIOError();
		char* result = AllocateString(buffer.str());
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(double));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(WriteFile)(void** args, void* ret) noexcept
	{
		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		const char* text = reinterpret_cast<const char*>(args[1u]);
		const std::filesystem::path path(file_name);

		std::ofstream file(path, std::ios::out | std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseWriteOpenFailure(path));
			std::memset(ret, 0, sizeof(double));
			return;
		}

		file.write(text, static_cast<std::streamsize>(std::strlen(text)));
		file.close();
		const bool success = !file.fail();
		if (success)
		{
			ClearLastIOError();
		}
		else
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to write file.");
		}

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = success;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(AppendToFile)(void** args, void* ret) noexcept
	{
		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		const char* text = reinterpret_cast<const char*>(args[1u]);
		const std::filesystem::path path(file_name);

		std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseWriteOpenFailure(path));
			std::memset(ret, 0, sizeof(double));
			return;
		}

		file.write(text, static_cast<std::streamsize>(std::strlen(text)));
		file.close();
		const bool success = !file.fail();
		if (success)
		{
			ClearLastIOError();
		}
		else
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to append to file.");
		}

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = success;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadBinaryFile)(void** args, void* ret) noexcept
	{
		struct FFIArray
		{
			void* data;
			int length;
		};

		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		const std::filesystem::path path(file_path);

		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseReadOpenFailure(path));
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		file.seekg(0, std::ios::end);
		const std::streamsize file_size = file.tellg();
		file.seekg(0, std::ios::beg);

		if (file_size < 0)
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to determine binary file size.");
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		if (file_size == 0)
		{
			ClearLastIOError();
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		double* array_data = static_cast<double*>(std::malloc(static_cast<size_t>(file_size) * sizeof(double)));
		if (array_data == nullptr)
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to allocate binary file buffer.");
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		std::vector<char> buffer(static_cast<size_t>(file_size));
		file.read(buffer.data(), file_size);
		if (file.fail())
		{
			std::free(array_data);
			SetLastIOError(IOErrorKind::Unknown, "Failed to read binary file.");
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		for (std::streamsize i = 0; i < file_size; i += 1)
		{
			const uint8_t byte_value = static_cast<uint8_t>(buffer[static_cast<size_t>(i)]);
			const int64_t value = static_cast<int64_t>(byte_value);
			std::memcpy(&array_data[i], &value, sizeof(double));
		}

		FFIArray* result = static_cast<FFIArray*>(std::malloc(sizeof(FFIArray)));
		if (result == nullptr)
		{
			std::free(array_data);
			SetLastIOError(IOErrorKind::Unknown, "Failed to allocate binary file result.");
			const int64_t null_ptr = 0;
			std::memcpy(ret, &null_ptr, sizeof(int64_t));
			return;
		}

		result->data = array_data;
		result->length = static_cast<int>(file_size);

		ClearLastIOError();
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(WriteBinaryFile)(void** args, void* ret) noexcept
	{
		struct ArrayArgument
		{
			void* data;
			int length;
		};

		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		ArrayArgument* array_arg = reinterpret_cast<ArrayArgument*>(args[1u]);
		if (array_arg == nullptr || array_arg->length < 0)
		{
			SetLastIOError(IOErrorKind::Unknown, "Invalid binary array argument.");
			std::memset(ret, 0, sizeof(double));
			return;
		}

		const std::filesystem::path path(file_name);
		std::ofstream file(path, std::ios::out | std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseWriteOpenFailure(path));
			std::memset(ret, 0, sizeof(double));
			return;
		}

		if (array_arg->length > 0 && array_arg->data == nullptr)
		{
			SetLastIOError(IOErrorKind::Unknown, "Binary array data was null.");
			std::memset(ret, 0, sizeof(double));
			return;
		}

		if (array_arg->length > 0)
		{
			double* array_data = reinterpret_cast<double*>(array_arg->data);
			std::vector<char> buffer(static_cast<size_t>(array_arg->length));
			for (int i = 0; i < array_arg->length; i += 1)
			{
				int64_t byte_value = 0;
				std::memcpy(&byte_value, &array_data[i], sizeof(double));
				buffer[static_cast<size_t>(i)] = static_cast<char>(byte_value & 0xFF);
			}
			file.write(buffer.data(), static_cast<std::streamsize>(array_arg->length));
		}

		file.close();
		const bool success = !file.fail();
		if (success)
		{
			ClearLastIOError();
		}
		else
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to write binary file.");
		}

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = success;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(FileExists)(void** args, void* ret) noexcept
	{
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const bool exists = std::filesystem::exists(file_path, ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = exists;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(DeleteFile)(void** args, void* ret) noexcept
	{
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const bool success = std::filesystem::remove(file_path, ec);
		if (!success && !ec)
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
		}

		SetLastIOErrorFromCode(ec);
		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = success && !ec;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(RenameFile)(void** args, void* ret) noexcept
	{
		const char* old_path = reinterpret_cast<const char*>(args[0u]);
		const char* new_path = reinterpret_cast<const char*>(args[1u]);
		std::error_code ec;
		std::filesystem::rename(old_path, new_path, ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = !ec;
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetFileSize)(void** args, void* ret) noexcept
	{
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const std::uintmax_t size = std::filesystem::file_size(file_path, ec);
		SetLastIOErrorFromCode(ec);

		const int64_t result = ec ? -1 : static_cast<int64_t>(size);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastIOErrorKind)(void**, void* ret) noexcept
	{
		const int64_t result = static_cast<int64_t>(s_last_io_error.m_kind);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastIOErrorMessage)(void**, void* ret) noexcept
	{
		char* result = s_last_io_error.m_message.empty()
			? AllocateEmptyString()
			: AllocateString(s_last_io_error.m_message);
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}
}
