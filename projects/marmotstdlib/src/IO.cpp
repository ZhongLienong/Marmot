#include "MarmotStdLibExports.h"

#include "FFIAllocation/FFIAllocation.h"
#include "Checksum.h"
#include "TextPath.h"
#include "ProcessState.h"
#include "WorkerErrors.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

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

	using FFIAllocation::AllocateString;
	using FFIAllocation::AllocateStringArray;

	std::mutex s_input_mutex;

#ifdef _WIN32
	void WriteStandardStream(DWORD stream, const char* text)
	{
		// Each statically linked CRT owns its descriptors. The current process
		// handle also sees redirection installed by the VM after loading us.
		const HANDLE handle = GetStdHandle(stream);
		DWORD written = 0;
		DWORD mode = 0;
		if (GetConsoleMode(handle, &mode) != 0)
		{
			const int size = MultiByteToWideChar(CP_UTF8, 0u, text, -1, nullptr, 0);
			if (size > 1)
			{
				std::wstring wide(static_cast<size_t>(size), L'\0');
				MultiByteToWideChar(CP_UTF8, 0u, text, -1, wide.data(), size);
				WriteConsoleW(handle, wide.data(), static_cast<DWORD>(size - 1), &written, nullptr);
			}
			return;
		}
		WriteFile(handle, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
	}
#endif

	void ClearLastIOError()
	{
		StdlibWorkerErrors::Status& status = StdlibWorkerErrors::Current().m_io;
		status.m_kind = static_cast<int64_t>(IOErrorKind::Success);
		status.m_message.clear();
	}

	void SetLastIOError(IOErrorKind kind, const std::string& message)
	{
		StdlibWorkerErrors::Status& status = StdlibWorkerErrors::Current().m_io;
		status.m_kind = static_cast<int64_t>(kind);
		status.m_message = message;
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

	MarmotFFIArray* ReadBinaryBytes(std::ifstream& file, std::streamsize size, bool whole_file)
	{
		if (size > std::numeric_limits<int32_t>::max())
		{
			SetLastIOError(IOErrorKind::Unknown, "Binary file exceeds the maximum array length. Read it in chunks.");
			return nullptr;
		}

		const std::unique_ptr<char[]> buffer(new (std::nothrow) char[static_cast<size_t>(size)]);
		if (!buffer)
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to allocate binary file buffer.");
			return nullptr;
		}

		file.read(buffer.get(), size);
		if (file.bad() || (file.fail() && (whole_file || !file.eof())))
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to read binary file.");
			return nullptr;
		}

		const int32_t length = static_cast<int32_t>(file.gcount());
		if (length == 0)
		{
			ClearLastIOError();
			return nullptr;
		}

		auto* values = static_cast<int64_t*>(std::malloc(static_cast<size_t>(length) * sizeof(int64_t)));
		auto* result = static_cast<MarmotFFIArray*>(std::malloc(sizeof(MarmotFFIArray)));
		if (values == nullptr || result == nullptr)
		{
			std::free(values);
			std::free(result);
			SetLastIOError(IOErrorKind::Unknown, "Failed to allocate binary file result.");
			return nullptr;
		}

		for (int32_t i = 0; i < length; i += 1)
		{
			values[i] = static_cast<uint8_t>(buffer[static_cast<size_t>(i)]);
		}
		*result = { values, length };
		ClearLastIOError();
		return result;
	}

	MarmotFFIArray* ReadBinaryFileBytes(
		const char* file_path, int64_t offset, std::optional<std::streamsize> max_bytes)
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const std::filesystem::path path = TextPath::FromText(file_path);
		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::status(path, ec);
		if (ec || std::filesystem::is_directory(status))
		{
			SetLastIOErrorFromCode(ec ? ec : std::make_error_code(std::errc::is_a_directory));
			return nullptr;
		}
		if (max_bytes && !std::filesystem::is_regular_file(status))
		{
			SetLastIOError(IOErrorKind::Unknown, "Binary chunks require a regular file.");
			return nullptr;
		}

		std::ifstream file(path, std::ios::binary);
		if (!file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseReadOpenFailure(path));
			return nullptr;
		}

		file.seekg(0, std::ios::end);
		const std::streamoff file_size = file.tellg();
		if (file_size < 0)
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to determine binary file size.");
			return nullptr;
		}
		if (offset >= file_size)
		{
			ClearLastIOError();
			return nullptr;
		}

		if (!file.seekg(offset, std::ios::beg))
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to seek to binary file offset.");
			return nullptr;
		}
		const std::streamsize available = file_size - offset;
		return ReadBinaryBytes(file, std::min(available, max_bytes.value_or(available)), !max_bytes);
	}
	void WriteBinaryBytes(void** args, void* ret, std::ios::openmode mode)
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		const auto* array_arg = static_cast<const MarmotFFIArray*>(args[1u]);
		if (array_arg == nullptr || array_arg->length < 0)
		{
			SetLastIOError(IOErrorKind::Unknown, "Invalid binary array argument.");
			std::memset(ret, 0, sizeof(double));
			return;
		}

		const std::filesystem::path path = TextPath::FromText(file_name);
		std::ofstream file(path, std::ios::out | std::ios::binary | mode);
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
			const auto* array_data = static_cast<const uint64_t*>(array_arg->data);
			std::vector<char> buffer(static_cast<size_t>(array_arg->length));
			for (int i = 0; i < array_arg->length; i += 1)
			{
				buffer[static_cast<size_t>(i)] = static_cast<char>(array_data[i] & 0xFFu);
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
}

extern "C"
{
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(Print)(void** args, void* ret) noexcept
	{
		const char* str = reinterpret_cast<const char*>(args[0u]);

		#ifdef __EMSCRIPTEN__
		std::cout << str;
		std::cout.flush();
		#elif defined(_WIN32)
		WriteStandardStream(STD_OUTPUT_HANDLE, str);
		#else
		std::printf("%s", str);
		std::fflush(stdout);
		#endif

		std::memset(ret, 0, sizeof(double));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(PrintError)(void** args, void* ret) noexcept
	{
		const char* str = reinterpret_cast<const char*>(args[0u]);

		#ifdef __EMSCRIPTEN__
		std::cerr << str;
		std::cerr.flush();
		#elif defined(_WIN32)
		WriteStandardStream(STD_ERROR_HANDLE, str);
		#else
		std::fprintf(stderr, "%s", str);
		std::fflush(stderr);
		#endif

		std::memset(ret, 0, sizeof(double));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ReadInput)(void**, void* ret) noexcept
	{
		const std::lock_guard<std::mutex> lock(s_input_mutex);
		std::ostringstream buffer;
		buffer << std::cin.rdbuf();
		char* result = AllocateString(buffer.str());

		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(double));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ReadLine)(void**, void* ret) noexcept
	{
		const std::lock_guard<std::mutex> lock(s_input_mutex);
		std::string line;
		if (std::getline(std::cin, line))
		{
			char* result = AllocateString(line);
			const int64_t ptr = reinterpret_cast<int64_t>(result);
			std::memcpy(ret, &ptr, sizeof(double));
		}
		else
		{
			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
		}
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(TryReadLine)(void**, void* ret) noexcept
	{
		const std::lock_guard<std::mutex> lock(s_input_mutex);
		std::string line;
		MarmotFFIArray* result = nullptr;
		if (std::getline(std::cin, line))
		{
			// The C-string FFI preserves embedded NUL through a segment boundary.
			std::vector<std::string> segments;
			size_t start = 0;
			for (size_t end = line.find('\0'); end != std::string::npos; end = line.find('\0', start))
			{
				segments.push_back(line.substr(start, end - start));
				start = end + 1;
			}
			segments.push_back(line.substr(start));
			result = AllocateStringArray(segments);
			if (result)
			{
				char** data = static_cast<char**>(result->data);
				if (std::find(data, data + result->length, nullptr) != data + result->length)
				{
					MARMOT_FFI_FreeResult(MARMOT_FFI_RETURN_ARRAY_STRINGS, result);
					result = nullptr;
				}
			}
			if (result)
			{
				ClearLastIOError();
			}
			else
			{
				SetLastIOError(IOErrorKind::Unknown, "Failed to allocate input line.");
			}
		}
		else if (std::cin.eof() && !std::cin.bad())
		{
			ClearLastIOError();
		}
		else
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to read a line.");
		}
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(ptr));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ReadFile)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		const std::filesystem::path path = TextPath::FromText(file_path);

		// Linux opens a directory as a stream that reads nothing.
		std::ifstream file(path, std::ios::in | std::ios::binary);
		if (std::filesystem::is_directory(path) || !file.is_open())
		{
			SetLastIOErrorFromCode(DiagnoseReadOpenFailure(path));
			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
			return;
		}

		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (file.bad())
		{
			SetLastIOError(IOErrorKind::Unknown, "Failed to read file.");
			char* empty = AllocateString("");
			const int64_t ptr = reinterpret_cast<int64_t>(empty);
			std::memcpy(ret, &ptr, sizeof(double));
			return;
		}

		ClearLastIOError();
		char* result = AllocateString(buffer.str());
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(double));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(WriteFile)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		const char* text = reinterpret_cast<const char*>(args[1u]);
		const std::filesystem::path path = TextPath::FromText(file_name);

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

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(AppendToFile)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_name = reinterpret_cast<const char*>(args[0u]);
		const char* text = reinterpret_cast<const char*>(args[1u]);
		const std::filesystem::path path = TextPath::FromText(file_name);

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

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ReadBinaryFile)(void** args, void* ret) noexcept
	{
		const char* path = reinterpret_cast<const char*>(args[0u]);
		const int64_t ptr = reinterpret_cast<int64_t>(ReadBinaryFileBytes(path, 0, std::nullopt));
		std::memcpy(ret, &ptr, sizeof(ptr));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ReadBinaryChunk)(void** args, void* ret) noexcept
	{
		const char* path = reinterpret_cast<const char*>(args[0u]);
		int64_t offset;
		int64_t max_bytes;
		std::memcpy(&offset, &args[1u], sizeof(offset));
		std::memcpy(&max_bytes, &args[2u], sizeof(max_bytes));

		MarmotFFIArray* result = nullptr;
		if (offset < 0 || max_bytes <= 0 || max_bytes > std::numeric_limits<int32_t>::max())
		{
			SetLastIOError(IOErrorKind::Unknown, "Expected a nonnegative offset and a byte count between 1 and 2147483647.");
		}
		else
		{
			result = ReadBinaryFileBytes(path, offset, max_bytes);
		}
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(ptr));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(WriteBinaryFile)(void** args, void* ret) noexcept
	{
		WriteBinaryBytes(args, ret, std::ios::trunc);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(AppendBinaryFile)(void** args, void* ret) noexcept
	{
		WriteBinaryBytes(args, ret, std::ios::app);
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(FileExists)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const bool exists = std::filesystem::exists(TextPath::FromText(file_path), ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = exists;
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(DeleteFile)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const bool success = std::filesystem::remove(TextPath::FromText(file_path), ec);
		if (!success && !ec)
		{
			ec = std::make_error_code(std::errc::no_such_file_or_directory);
		}

		SetLastIOErrorFromCode(ec);
		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = success && !ec;
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(RenameFile)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* old_path = reinterpret_cast<const char*>(args[0u]);
		const char* new_path = reinterpret_cast<const char*>(args[1u]);
		std::error_code ec;
		std::filesystem::rename(TextPath::FromText(old_path), TextPath::FromText(new_path), ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = !ec;
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetFileSize)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* file_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const std::uintmax_t size = std::filesystem::file_size(TextPath::FromText(file_path), ec);
		SetLastIOErrorFromCode(ec);

		const int64_t result = ec ? -1 : static_cast<int64_t>(size);
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	// Sorted, so a listing is the same on every machine.
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(ListDirectory)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* directory_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		std::vector<std::string> names;
		for (std::filesystem::directory_iterator entry(TextPath::FromText(directory_path), ec); !ec && entry != std::filesystem::directory_iterator(); entry.increment(ec))
		{
			names.push_back(TextPath::ToText(entry->path().filename()));
		}
		std::ranges::sort(names);
		SetLastIOErrorFromCode(ec);

		const int64_t ptr = ec ? 0 : reinterpret_cast<int64_t>(AllocateStringArray(names));
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	// Makes the missing parents too. A directory already there is made.
	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(MakeDirectory)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* directory_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		std::filesystem::create_directories(TextPath::FromText(directory_path), ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = !ec;
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(DirectoryExists)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const char* directory_path = reinterpret_cast<const char*>(args[0u]);
		std::error_code ec;
		const bool exists = std::filesystem::is_directory(TextPath::FromText(directory_path), ec);
		SetLastIOErrorFromCode(ec);

		std::memset(ret, 0, sizeof(double));
		*reinterpret_cast<bool*>(ret) = exists;
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(FileChecksum)(void** args, void* ret) noexcept
	{
		const std::shared_lock<std::shared_mutex> lock(MarmotProcessState::s_mutex);
		const std::filesystem::path path = TextPath::FromText(reinterpret_cast<const char*>(args[0u]));
		// Linux opens a directory as a stream that reads nothing.
		const std::expected<std::string, std::string> checksum = std::filesystem::is_directory(path)
			? std::unexpected(std::string())
			: MarmotChecksum::HashFile(path);
		if (checksum.has_value())
		{
			ClearLastIOError();
		}
		else
		{
			SetLastIOErrorFromCode(DiagnoseReadOpenFailure(path));
		}

		char* result = AllocateString(checksum.value_or(""));
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetLastIOErrorKind)(void**, void* ret) noexcept
	{
		const int64_t result = StdlibWorkerErrors::Current().m_io.m_kind;
		std::memcpy(ret, &result, sizeof(int64_t));
	}

	MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FUNC(GetLastIOErrorMessage)(void**, void* ret) noexcept
	{
		char* result = AllocateString(StdlibWorkerErrors::Current().m_io.m_message);
		const int64_t ptr = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &ptr, sizeof(int64_t));
	}
}
