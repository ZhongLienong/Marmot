#include "Utility/Driver/MidoriDriver.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Compiler/Compiler.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Interpreter/Worker/Worker.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

namespace
{
	[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(const char* name)
	{
#ifdef _WIN32
		char* value = nullptr;
		size_t length = 0u;
		if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
		{
			return std::nullopt;
		}

		std::string result(value);
		free(value);
		return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
#else
		const char* value = std::getenv(name);
		return value == nullptr || value[0] == '\0' ? std::nullopt : std::optional<std::string>(value);
#endif
	}

	// A PATH-style list: `;` separated on Windows, `:` elsewhere.
	[[nodiscard]] std::vector<std::filesystem::path> SplitPathList(const std::string& value)
	{
#ifdef _WIN32
		const char separator = ';';
#else
		const char separator = ':';
#endif
		std::vector<std::filesystem::path> paths;
		for (const std::ranges::subrange<std::string::const_iterator> segment : value | std::views::split(separator))
		{
			if (!segment.empty())
			{
				paths.emplace_back(std::string(segment.begin(), segment.end()));
			}
		}
		return paths;
	}

	[[nodiscard]] bool ShouldEmitMachineReadableWarnings()
	{
#ifdef _WIN32
		char* warning_format = nullptr;
		size_t warning_format_length = 0u;
		const errno_t result = _dupenv_s(&warning_format, &warning_format_length, "MARMOT_TEST_WARNING_FORMAT");
		if (result != 0 || warning_format == nullptr)
		{
			return false;
		}

		const bool enabled = std::string_view(warning_format) == "machine";
		free(warning_format);
		return enabled;
#else
		const char* warning_format = std::getenv("MARMOT_TEST_WARNING_FORMAT");
		return warning_format != nullptr && std::string_view(warning_format) == "machine";
#endif
	}

	void EmitWarnings(const MidoriResult::CompilerReport& report)
	{
		if (!report.HasWarnings())
		{
			return;
		}

		std::print("{}", report.RenderedWarnings());
		if (ShouldEmitMachineReadableWarnings())
		{
			std::print("{}", report.MachineReadableWarnings());
		}
	}
}

namespace MidoriDriver
{
	DriverError DriverError::FileSystem(std::string message)
	{
		DriverError error;
		error.m_message = std::move(message);
		return error;
	}

	DriverError DriverError::Compilation(MidoriResult::CompilerReport report)
	{
		DriverError error;
		error.m_report = std::move(report);
		error.m_is_compilation_failure = true;
		return error;
	}

	DriverError DriverError::Compilation(MidoriResult::CompilerDiagnostics diagnostics)
	{
		return Compilation(MidoriResult::CompilerReport(std::move(diagnostics)));
	}

	DriverError DriverError::Diagnostics(MidoriResult::CompilerReport report)
	{
		DriverError error;
		error.m_report = std::move(report);
		return error;
	}

	DriverError DriverError::Diagnostics(MidoriResult::CompilerDiagnostics diagnostics)
	{
		return Diagnostics(MidoriResult::CompilerReport(std::move(diagnostics)));
	}

	std::string DriverError::Rendered() const
	{
		if (m_report.has_value())
		{
			std::string rendered;
			if (m_is_compilation_failure)
			{
				rendered = "Compilation failed :( \n";
			}

			rendered += m_report->Rendered();
			if (ShouldEmitMachineReadableWarnings())
			{
				rendered += m_report->MachineReadableWarnings();
			}
			return rendered;
		}

		return m_message;
	}

	SourceReadResult ReadSourceFile(const std::filesystem::path& file_path)
	{
		std::ifstream file(file_path, std::ios::binary);
		if (!file.is_open())
		{
			return std::unexpected(DriverError::FileSystem(std::format("Could not open file: {}\n", file_path.string())));
		}

		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (!buffer)
		{
			return std::unexpected(DriverError::FileSystem(std::format("Could not read file to buffer: {}\n", file_path.string())));
		}

		return buffer.str();
	}

	std::vector<std::filesystem::path> EnvironmentLibraryPaths()
	{
		const std::optional<std::string> value = ReadEnvironmentVariable("MARMOT_LIBRARY_PATH");
		return value.has_value() ? SplitPathList(value.value()) : std::vector<std::filesystem::path>{};
	}

	std::vector<std::filesystem::path> EnvironmentSearchPaths()
	{
		const std::optional<std::string> value = ReadEnvironmentVariable("MARMOT_PATH");
		return value.has_value() ? SplitPathList(value.value()) : std::vector<std::filesystem::path>{};
	}

	CompilationInputs EnvironmentCompilationInputs()
	{
		return CompilationInputs().WithSearchPaths(EnvironmentSearchPaths());
	}

	MidoriResult::CompilerResult CompileSource(std::string source_code, std::string file_name)
	{
		MidoriResult::CompilationResult compile_result = CompileSourceWithReport(std::move(source_code), std::move(file_name));
		if (!compile_result.has_value())
		{
			// Legacy callers still depend on the executable-or-errors shape; preserve
			// the report on the new path and narrow only here.
			return std::unexpected(std::move(compile_result.error()).TakeErrors());
		}

		return std::move(compile_result.value()).TakeExecutable();
	}

	MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name)
	{
		return CompileSourceWithReport(std::move(source_code), std::move(file_name), EnvironmentCompilationInputs());
	}

	MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name, CompilationInputs inputs)
	{
		return Compiler(std::move(source_code), std::move(file_name), std::move(inputs)).CompileWithReport();
	}

	CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path)
	{
		return CompileFileWithReport(file_path, EnvironmentCompilationInputs());
	}

	CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path, CompilationInputs inputs)
	{
		SourceReadResult source_result = ReadSourceFile(file_path);
		if (!source_result.has_value())
		{
			return std::unexpected(std::move(source_result.error()));
		}

		MidoriResult::CompilationResult compile_result = CompileSourceWithReport(std::move(source_result.value()), file_path.string(), std::move(inputs));
		if (!compile_result.has_value())
		{
			return std::unexpected(DriverError::Compilation(std::move(compile_result.error())));
		}

		return std::move(compile_result).value();
	}

	CompileFileResult CompileFile(const std::filesystem::path& file_path)
	{
		CompileFileWithReportResult compile_result = CompileFileWithReport(file_path);
		if (!compile_result.has_value())
		{
			return std::unexpected(std::move(compile_result.error()));
		}

		return std::move(compile_result.value()).TakeExecutable();
	}

	LoadArtifactResult LoadArtifact(const std::filesystem::path& path)
	{
		std::expected<MidoriExecutable, std::string> load_result = MidoriBinaryArtifact::ReadExecutableFromFile(path);
		if (!load_result.has_value())
		{
			return std::unexpected(DriverError::FileSystem(load_result.error()));
		}
		return std::move(load_result.value());
	}

	std::expected<void, DriverError> LoadNativeLibraries(const MidoriExecutable& executable, const NativeLibraryOptions& options)
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

			const std::unordered_map<std::string, NativeLibrarySettings>::const_iterator configured = options.m_libraries.find(library.m_name);
			const NativeLibrarySettings settings = configured != options.m_libraries.end() ? configured->second : NativeLibrarySettings{};
			const std::string file_name = prefix + library.m_name + extension;

			std::vector<std::filesystem::path> candidates;
			if (settings.m_path.has_value())
			{
				candidates.push_back(settings.m_path.value());
			}
			for (const std::filesystem::path& directory : options.m_search_paths)
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
				return std::unexpected(DriverError::Compilation(MidoriResult::CompilerDiagnostics(CompilerError::Simple(
					CompilerStage::Module,
					std::format("FFI error: native library '{}' not found. Looked for:{}\nPass --library-path <dir>, set MARMOT_LIBRARY_PATH, or run through `marmot run`.", library.m_name, searched)))));
			}

			std::unordered_map<std::string, std::string> functions;
			for (const std::string& symbol : library.m_symbols)
			{
				functions.emplace(library.m_name + NATIVE_SYMBOL_SEPARATOR + symbol, symbol);
			}

			std::optional<std::string_view> expected_checksum = std::nullopt;
			if (settings.m_checksum.has_value())
			{
				expected_checksum = settings.m_checksum.value();
			}

			const std::expected<void, std::string> load_result =
				cache.LoadLibraryWithFunctions(*found, library.m_name, functions, settings.m_thread_safe, expected_checksum);
			if (!load_result.has_value())
			{
				return std::unexpected(DriverError::Compilation(MidoriResult::CompilerDiagnostics(CompilerError::Simple(CompilerStage::Module, load_result.error()))));
			}
		}

		return {};
	}

	RunResult RunExecutable(MidoriExecutable&& executable)
	{
		VirtualMachine vm(std::move(executable));
		RunResult run_result = vm.Execute();
		WorkerRegistry::GetInstance().Shutdown();
		return run_result;
	}

	DriverResult CompileAndRunFile(const std::filesystem::path& file_path)
	{
		CompileFileWithReportResult compile_result = CompileFileWithReport(file_path);
		if (!compile_result.has_value())
		{
			return std::unexpected(std::move(compile_result.error()));
		}

		MidoriResult::CompiledProgram compiled_program = std::move(compile_result).value();
		EmitWarnings(compiled_program.Report());

		std::expected<void, DriverError> load_result = LoadNativeLibraries(compiled_program.m_executable, NativeLibraryOptions{ .m_search_paths = EnvironmentLibraryPaths() });
		if (!load_result.has_value())
		{
			return std::unexpected(std::move(load_result.error()));
		}

		RunResult run_result = RunExecutable(std::move(compiled_program).TakeExecutable());
		if (!run_result.has_value())
		{
			return std::unexpected(DriverError::Diagnostics(MidoriResult::CompilerDiagnostics(run_result.error().ToCompilerError())));
		}

		return run_result.value();
	}
}
