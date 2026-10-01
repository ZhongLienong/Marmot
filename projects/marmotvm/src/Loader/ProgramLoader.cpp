#include "Loader/ProgramLoader.h"

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Interpreter/Worker/Worker.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
	// The directories in the PATH-style variable `name`: `;` separated on
	// Windows, `:` elsewhere, empty entries dropped.
	[[nodiscard]] std::vector<std::filesystem::path> ReadPathList(const char* name)
	{
#ifdef _WIN32
		constexpr char separator = ';';
		char* raw = nullptr;
		size_t length = 0u;
		if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr)
		{
			return {};
		}
		const std::string value(raw);
		free(raw);
#else
		constexpr char separator = ':';
		const char* raw = std::getenv(name);
		if (raw == nullptr)
		{
			return {};
		}
		const std::string value(raw);
#endif
		using Segment = std::ranges::subrange<std::string::const_iterator>;
		return value
			| std::views::split(separator)
			| std::views::filter([](const Segment& segment) { return !segment.empty(); })
			| std::views::transform([](const Segment& segment) { return std::filesystem::path(std::string(segment.begin(), segment.end())); })
			| std::ranges::to<std::vector<std::filesystem::path>>();
	}
}

namespace MidoriProgramLoader
{
	std::vector<std::filesystem::path> EnvironmentLibraryPaths()
	{
		return ReadPathList("MARMOT_LIBRARY_PATH");
	}

	std::expected<VmExecutable, std::string> ReadProgram(const std::filesystem::path& path)
	{
		return VmBinaryArtifact::ReadExecutableFromFile(path);
	}

	std::expected<void, std::string> LoadNativeLibraries(const VmExecutable& executable, const NativeLibraryLocations& locations)
	{
#ifdef _WIN32
		const std::string prefix;
		const std::string extension = ".dll";
		const std::filesystem::path platform_directory = std::filesystem::path("lib") / "windows" / "x64";
#elif defined(__APPLE__)
		const std::string prefix = "lib";
		const std::string extension = ".dylib";
		const std::filesystem::path platform_directory = std::filesystem::path("lib") / "macos";
#else
		const std::string prefix = "lib";
		const std::string extension = ".so";
		const std::filesystem::path platform_directory = std::filesystem::path("lib") / "linux" / "x86_64";
#endif

		SharedLibraryCache& cache = SharedLibraryCache::GetInstance();
		for (const VmNativeLibraryImport& library : executable.GetNativeLibraries())
		{
			if (cache.IsLibraryLoaded(library.m_name))
			{
				continue;
			}

			const std::string file_name = prefix + library.m_name + extension;
			std::vector<std::filesystem::path> candidates;
			const std::unordered_map<std::string, std::filesystem::path>::const_iterator file = locations.m_files.find(library.m_name);
			if (file != locations.m_files.end())
			{
				candidates.push_back(file->second);
			}
			for (const std::filesystem::path& directory : locations.m_search_paths)
			{
				candidates.push_back(directory / file_name);
			}
			for (const std::string& directory : library.m_hint_directories)
			{
				candidates.push_back(std::filesystem::path(directory) / platform_directory / file_name);
				candidates.push_back(std::filesystem::path(directory) / file_name);
			}

			const std::vector<std::filesystem::path>::const_iterator found = std::ranges::find_if(
				candidates,
				[](const std::filesystem::path& candidate)
				{
					std::error_code error;
					return std::filesystem::is_regular_file(candidate, error);
				});
			if (found == candidates.end())
			{
				std::string searched;
				for (const std::filesystem::path& candidate : candidates)
				{
					searched += std::format("\n  {}", candidate.string());
				}
				return std::unexpected(std::format(
					"FFI error: native library '{}' not found. Looked for:{}\nPass --library {}=<file> or --library-path <dir>, set MARMOT_LIBRARY_PATH, or run through `marmot run`.",
					library.m_name,
					searched,
					library.m_name));
			}

			std::unordered_map<std::string, std::string> functions;
			for (const std::string& symbol : library.m_symbols)
			{
				functions.emplace(library.m_name + VM_NATIVE_SYMBOL_SEPARATOR + symbol, symbol);
			}

			std::optional<std::string_view> expected_checksum = std::nullopt;
			if (library.m_policy.m_checksum.has_value())
			{
				expected_checksum = library.m_policy.m_checksum.value();
			}

			const std::expected<void, std::string> load_result =
				cache.LoadLibraryWithFunctions(*found, library.m_name, functions, library.m_policy.m_thread_safe, expected_checksum);
			if (!load_result.has_value())
			{
				return std::unexpected(load_result.error());
			}
		}

		return {};
	}

	std::expected<int, RuntimeError> Run(VmExecutable&& executable)
	{
		VirtualMachine vm(std::move(executable));
		std::expected<int, RuntimeError> run_result = vm.Execute();
		WorkerRegistry::GetInstance().Shutdown();
		return run_result;
	}
}
