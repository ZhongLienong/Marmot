#include "Loader/ProgramLoader.h"

#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Common/Environment/Environment.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Interpreter/Worker/Worker.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string_view>
#include <system_error>

namespace MidoriProgramLoader
{
	std::vector<std::filesystem::path> EnvironmentLibraryPaths()
	{
		return MidoriEnvironment::ReadPathList("MARMOT_LIBRARY_PATH");
	}

	std::expected<MidoriExecutable, std::string> ReadProgram(const std::filesystem::path& path)
	{
		return MidoriBinaryArtifact::ReadExecutableFromFile(path);
	}

	std::expected<void, std::string> LoadNativeLibraries(const MidoriExecutable& executable, const NativeLibraryLocations& locations)
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
		for (const NativeLibraryImport& library : executable.GetNativeLibraries())
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
				functions.emplace(library.m_name + NATIVE_SYMBOL_SEPARATOR + symbol, symbol);
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

	std::expected<int, RuntimeError> Run(MidoriExecutable&& executable)
	{
		VirtualMachine vm(std::move(executable));
		std::expected<int, RuntimeError> run_result = vm.Execute();
		WorkerRegistry::GetInstance().Shutdown();
		return run_result;
	}
}
