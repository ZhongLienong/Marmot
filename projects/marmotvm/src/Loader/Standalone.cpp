#include "Loader/Standalone.h"

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"
#include "Loader/ProgramLoader.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace
{
	constexpr std::array<char, 16u> s_magic = {
		'M', 'A', 'R', 'M', 'O', 'T', '-', 'E', 'X', 'E', '-', 'v', '1', '\0', '\0', '\0'
	};
	constexpr std::streamoff s_footer_size = 24;

	class PackageReader
	{
		std::span<const char> m_remaining;

	public:
		explicit PackageReader(std::span<const char> bytes)
			: m_remaining(bytes)
		{
		}

		[[nodiscard]] size_t Remaining() const
		{
			return m_remaining.size();
		}

		[[nodiscard]] std::expected<uint64_t, std::string> ReadSize()
		{
			if (m_remaining.size() < 8u)
			{
				return std::unexpected("Truncated standalone executable: missing field length.");
			}
			uint64_t value = 0u;
			for (size_t index = 0u; index < 8u; index += 1u)
			{
				value |= static_cast<uint64_t>(static_cast<unsigned char>(m_remaining[index])) << (index * 8u);
			}
			m_remaining = m_remaining.subspan(8u);
			return value;
		}

		[[nodiscard]] std::expected<std::string, std::string> ReadBlob()
		{
			const std::expected<uint64_t, std::string> size = ReadSize();
			if (!size.has_value())
			{
				return std::unexpected(size.error());
			}
			if (size.value() > m_remaining.size())
			{
				return std::unexpected("Truncated standalone executable: field exceeds the package.");
			}
			std::string bytes(m_remaining.data(), static_cast<size_t>(size.value()));
			m_remaining = m_remaining.subspan(static_cast<size_t>(size.value()));
			return bytes;
		}
	};

	class ExtractedLibraries
	{
		std::filesystem::path m_directory;
		SharedLibraryCache& m_cache;

	public:
		ExtractedLibraries()
			: m_cache(SharedLibraryCache::GetInstance())
		{
		}

		~ExtractedLibraries()
		{
			// DLLs must be unloaded before Windows can remove their files.
			m_cache.Clear();
			if (!m_directory.empty())
			{
				std::error_code error;
				std::filesystem::remove_all(m_directory, error);
			}
		}

		[[nodiscard]] std::expected<MidoriProgramLoader::NativeLibraryLocations, std::string> Write(
			const std::vector<MidoriStandalone::NativeLibrary>& libraries)
		{
			std::error_code error;
			const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
			if (error)
			{
				return std::unexpected(std::format("Cannot find the temporary directory: {}", error.message()));
			}
#ifdef _WIN32
			const unsigned long process_id = GetCurrentProcessId();
#elif defined(__linux__)
			const int process_id = getpid();
#else
			const int process_id = 0;
#endif
			const std::filesystem::path directory = temporary / std::format(
				"marmot-libraries-{}-{}", process_id, std::chrono::steady_clock::now().time_since_epoch().count());
			if (!std::filesystem::create_directory(directory, error))
			{
				return std::unexpected(std::format("Cannot create {}: {}", directory.string(), error.message()));
			}
			m_directory = directory;
			std::filesystem::permissions(directory, std::filesystem::perms::owner_all, error);
			if (error)
			{
				return std::unexpected(std::format("Cannot set permissions on {}: {}", directory.string(), error.message()));
			}

			MidoriProgramLoader::NativeLibraryLocations locations;
			for (size_t index = 0u; index < libraries.size(); index += 1u)
			{
				const MidoriStandalone::NativeLibrary& library = libraries[index];
				const std::filesystem::path library_directory = directory / std::to_string(index);
				std::filesystem::create_directory(library_directory, error);
				if (error)
				{
					return std::unexpected(std::format("Cannot create {}: {}", library_directory.string(), error.message()));
				}
				const std::filesystem::path path = library_directory / library.m_file_name;
				std::ofstream out(path, std::ios::binary);
				out.write(library.m_bytes.data(), static_cast<std::streamsize>(library.m_bytes.size()));
				out.close();
				if (!out)
				{
					return std::unexpected(std::format("Cannot extract native library {}", path.string()));
				}
				locations.m_files.emplace(library.m_name, path);
			}
			return locations;
		}
	};

	[[nodiscard]] std::expected<MidoriStandalone::Package, std::string> ReadPackage(std::span<const char> bytes)
	{
		PackageReader reader(bytes);
		std::expected<std::string, std::string> bytecode = reader.ReadBlob();
		if (!bytecode.has_value())
		{
			return std::unexpected(bytecode.error());
		}
		std::istringstream program_stream(std::move(bytecode).value());
		std::expected<VmExecutable, std::string> program = VmBinaryArtifact::ReadExecutable(program_stream);
		if (!program.has_value())
		{
			return std::unexpected(program.error());
		}

		const std::expected<uint64_t, std::string> count = reader.ReadSize();
		if (!count.has_value())
		{
			return std::unexpected(count.error());
		}
		if (count.value() > reader.Remaining() / 24u)
		{
			return std::unexpected("Truncated standalone executable: invalid native library count.");
		}
		std::vector<MidoriStandalone::NativeLibrary> libraries;
		for (uint64_t index = 0u; index < count.value(); index += 1u)
		{
			std::expected<std::string, std::string> name = reader.ReadBlob();
			if (!name.has_value())
			{
				return std::unexpected(name.error());
			}
			std::expected<std::string, std::string> file_name = reader.ReadBlob();
			if (!file_name.has_value())
			{
				return std::unexpected(file_name.error());
			}
			std::expected<std::string, std::string> contents = reader.ReadBlob();
			if (!contents.has_value())
			{
				return std::unexpected(contents.error());
			}
			const std::filesystem::path path(std::u8string(file_name->begin(), file_name->end()));
			if (path.empty() || path != path.filename() || path == "." || path == ".." ||
				file_name->find('\0') != std::string::npos)
			{
				return std::unexpected("Corrupt standalone executable: invalid native library file name.");
			}
			libraries.emplace_back(std::move(name).value(), path, std::move(contents).value());
		}
		if (reader.Remaining() != 0u)
		{
			return std::unexpected("Corrupt standalone executable: trailing package bytes.");
		}
		if (libraries.size() != program->GetNativeLibraries().size())
		{
			return std::unexpected("Corrupt standalone executable: native library imports do not match the bundle.");
		}
		for (const VmNativeLibraryImport& imported : program->GetNativeLibraries())
		{
			if (std::ranges::count(libraries, imported.m_name, &MidoriStandalone::NativeLibrary::m_name) != 1)
			{
				return std::unexpected(std::format("Corrupt standalone executable: native library '{}' is missing or repeated.", imported.m_name));
			}
		}
		return MidoriStandalone::Package(std::move(program).value(), std::move(libraries));
	}
}

MidoriStandalone::NativeLibrary::NativeLibrary(std::string name, std::filesystem::path file_name, std::string bytes)
	: m_name(std::move(name)), m_file_name(std::move(file_name)), m_bytes(std::move(bytes))
{
}

MidoriStandalone::Package::Package(VmExecutable program, std::vector<NativeLibrary> libraries)
	: m_program(std::move(program)), m_libraries(std::move(libraries))
{
}

std::expected<std::optional<MidoriStandalone::Package>, std::string> MidoriStandalone::Read(std::istream& image)
{
	image.seekg(0, std::ios::end);
	const std::streamoff image_size = image.tellg();
	if (image_size < 0)
	{
		return std::unexpected("Cannot read the running executable size.");
	}
	if (image_size < s_footer_size)
	{
		return std::nullopt;
	}
	image.seekg(-static_cast<std::streamoff>(s_magic.size()), std::ios::end);
	std::array<char, 16u> magic;
	image.read(magic.data(), magic.size());
	if (!image)
	{
		return std::unexpected("Cannot read the standalone executable footer.");
	}
	if (magic != s_magic)
	{
		return std::nullopt;
	}
	image.seekg(-s_footer_size, std::ios::end);
	std::array<char, 8u> length;
	image.read(length.data(), length.size());
	if (!image)
	{
		return std::unexpected("Cannot read the standalone executable size.");
	}
	PackageReader footer(length);
	const uint64_t size = footer.ReadSize().value();
	if (size > static_cast<uint64_t>(image_size - s_footer_size))
	{
		return std::unexpected("Truncated standalone executable: package exceeds the image.");
	}
	image.seekg(image_size - s_footer_size - static_cast<std::streamoff>(size));
	std::string bytes(static_cast<size_t>(size), '\0');
	image.read(bytes.data(), static_cast<std::streamsize>(size));
	if (!image)
	{
		return std::unexpected("Cannot read the standalone executable package.");
	}
	return ReadPackage(bytes).transform([](Package package) {
		return std::optional<Package>(std::move(package));
	});
}

std::expected<std::filesystem::path, std::string> MidoriStandalone::CurrentExecutablePath()
{
#ifdef _WIN32
	std::wstring buffer(32768u, L'\0');
	const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0u || length == buffer.size())
	{
		return std::unexpected(std::format("Cannot locate the running executable (Windows error {}).", GetLastError()));
	}
	buffer.resize(length);
	return std::filesystem::path(buffer);
#elif defined(__linux__)
	std::error_code error;
	const std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
	if (error)
	{
		return std::unexpected(std::format("Cannot locate the running executable: {}", error.message()));
	}
	return path;
#else
	return std::unexpected("Standalone executables currently support Windows and Linux.");
#endif
}

std::expected<std::optional<MidoriStandalone::Package>, std::string> MidoriStandalone::ReadCurrentExecutable()
{
#if defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
	const std::expected<std::filesystem::path, std::string> path = CurrentExecutablePath();
	if (!path.has_value())
	{
		return std::unexpected(path.error());
	}
	std::ifstream image(path.value(), std::ios::binary);
#else
	std::ifstream image("/proc/self/exe", std::ios::binary);
#endif
	if (!image)
	{
		return std::unexpected("Cannot read the running executable.");
	}
	return Read(image);
#else
	return std::nullopt;
#endif
}

std::expected<int, std::string> MidoriStandalone::Run(Package&& package)
{
	if (!package.m_libraries.empty())
	{
		// Static lifetime also cleans up when a program calls System::Exit.
		static ExtractedLibraries extracted;
		const std::expected<MidoriProgramLoader::NativeLibraryLocations, std::string> locations =
			extracted.Write(package.m_libraries);
		if (!locations.has_value())
		{
			return std::unexpected(locations.error());
		}
		const std::expected<void, std::string> loaded =
			MidoriProgramLoader::LoadNativeLibraries(package.m_program, locations.value());
		if (!loaded.has_value())
		{
			return std::unexpected(loaded.error());
		}
	}
	const std::expected<int, RuntimeError> result = MidoriProgramLoader::Run(std::move(package.m_program));
	if (!result.has_value())
	{
		std::print("{}", result.error().Rendered());
		return result.error().ExitCode();
	}
	return result.value();
}
