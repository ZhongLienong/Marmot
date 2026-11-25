#include "Utility/Driver/MarmotDriver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <ranges>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Compiler/Compiler.h"
#include "Support/Expected.h"

namespace
{
	// The directories in the PATH-style variable `name`: `;` separated on
	// Windows, `:` elsewhere, empty entries dropped.
	[[nodiscard]] std::vector<std::filesystem::path> ReadPathList(const char* name)
	{
#ifdef _WIN32
		constexpr char separator = ';';
#else
		constexpr char separator = ':';
#endif
		const char* raw = std::getenv(name);
		const std::string value = raw == nullptr ? std::string() : std::string(raw);
		using Segment = std::ranges::subrange<std::string::const_iterator>;
		return value
			| std::views::split(separator)
			| std::views::filter([](const Segment& segment) { return !segment.empty(); })
			| std::views::transform([](const Segment& segment) { return std::filesystem::path(std::string(segment.begin(), segment.end())); })
			| std::ranges::to<std::vector<std::filesystem::path>>();
	}

	// The source file could not be read, so nothing was compiled.
	[[nodiscard]] MarmotDriver::DriverError FileSystemError(std::string_view message, const std::filesystem::path& file_path)
	{
		MarmotDriver::DriverError error;
		error.m_report = MarmotResult::CompilerReport(CompilerError::WithFile(CompilerStage::Compiler, message, file_path.string()));
		return error;
	}

	[[nodiscard]] MarmotCompiler::Expected<std::string, MarmotDriver::DriverError> ReadSourceFile(const std::filesystem::path& file_path, const CompilationInputs& inputs)
	{
		if (const std::string* overlay = inputs.SourceOverlay(file_path); overlay != nullptr)
		{
			return *overlay;
		}
		// Linux opens a directory as a stream that reads nothing.
		std::ifstream file(file_path, std::ios::binary);
		if (std::filesystem::is_directory(file_path) || !file.is_open())
		{
			return MarmotCompiler::Failure(FileSystemError("Could not open file", file_path));
		}

		// Not `!buffer`: inserting an empty file's rdbuf sets failbit, and an
		// empty file is a program.
		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (file.bad())
		{
			return MarmotCompiler::Failure(FileSystemError("Could not read file", file_path));
		}

		return buffer.str();
	}
}

namespace MarmotDriver
{
	bool ShouldEmitMachineReadableWarnings()
	{
		const char* warning_format = std::getenv("MARMOT_TEST_WARNING_FORMAT");
		return warning_format != nullptr && std::string_view(warning_format) == "machine";
	}

	DriverError DriverError::Compilation(MarmotResult::CompilerReport report)
	{
		DriverError error;
		error.m_report = std::move(report);
		error.m_is_compilation_failure = true;
		return error;
	}

	std::string DriverError::Rendered() const
	{
		std::string rendered = m_is_compilation_failure ? "Compilation failed :( \n" : "";
		rendered += m_report.Rendered();
		if (ShouldEmitMachineReadableWarnings())
		{
			rendered += m_report.MachineReadableWarnings();
		}
		return rendered;
	}

	CompilationInputs EnvironmentCompilationInputs()
	{
		return CompilationInputs().WithSearchPaths(ReadPathList("MARMOT_PATH"));
	}

	MarmotResult::CompiledProgramReportResult CompileSourceWithReport(std::string source_code, std::string file_name)
	{
		return CompileSourceWithReport(std::move(source_code), std::move(file_name), EnvironmentCompilationInputs());
	}

	MarmotResult::CompiledProgramReportResult CompileSourceWithReport(std::string source_code, std::string file_name, CompilationInputs inputs)
	{
		return Compiler(std::move(source_code), std::move(file_name), std::move(inputs)).CompileWithReport();
	}

	CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path)
	{
		return CompileFileWithReport(file_path, EnvironmentCompilationInputs());
	}

	CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path, CompilationInputs inputs)
	{
		MarmotCompiler::Expected<std::string, DriverError> source_result = ReadSourceFile(file_path, inputs);
		if (!source_result.HasValue())
		{
			return MarmotCompiler::Failure(std::move(source_result.Error()));
		}

		MarmotResult::CompiledProgramReportResult compile_result = CompileSourceWithReport(std::move(source_result.Value()), file_path.string(), std::move(inputs));
		if (!compile_result.HasValue())
		{
			return MarmotCompiler::Failure(DriverError::Compilation(std::move(compile_result.Error())));
		}

		return std::move(compile_result).Value();
	}

	EditorAnalysis AnalyzeFile(const std::filesystem::path& file_path, CompilationInputs inputs)
	{
		MarmotCompiler::Expected<std::string, DriverError> source = ReadSourceFile(file_path, inputs);
		if (!source)
		{
			return { std::move(source.Error().m_report), {} };
		}
		return Compiler(std::move(source).Value(), file_path.string(), std::move(inputs)).Analyze();
	}
}
