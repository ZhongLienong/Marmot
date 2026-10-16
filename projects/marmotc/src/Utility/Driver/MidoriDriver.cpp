#include "Utility/Driver/MidoriDriver.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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
#include <vector>

#include "Compiler/Compiler.h"

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
	[[nodiscard]] MidoriDriver::DriverError FileSystemError(std::string_view message, const std::filesystem::path& file_path)
	{
		MidoriDriver::DriverError error;
		error.m_report = MidoriResult::CompilerReport(CompilerError::WithFile(CompilerStage::Compiler, message, file_path.string()));
		return error;
	}

	[[nodiscard]] std::expected<std::string, MidoriDriver::DriverError> ReadSourceFile(const std::filesystem::path& file_path)
	{
		// Linux opens a directory as a stream that reads nothing.
		std::ifstream file(file_path, std::ios::binary);
		if (std::filesystem::is_directory(file_path) || !file.is_open())
		{
			return std::unexpected(FileSystemError("Could not open file", file_path));
		}

		// Not `!buffer`: inserting an empty file's rdbuf sets failbit, and an
		// empty file is a program.
		std::ostringstream buffer;
		buffer << file.rdbuf();
		if (file.bad())
		{
			return std::unexpected(FileSystemError("Could not read file", file_path));
		}

		return buffer.str();
	}
}

namespace MidoriDriver
{
	bool ShouldEmitMachineReadableWarnings()
	{
		const char* warning_format = std::getenv("MARMOT_TEST_WARNING_FORMAT");
		return warning_format != nullptr && std::string_view(warning_format) == "machine";
	}

	DriverError DriverError::Compilation(MidoriResult::CompilerReport report)
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
		std::expected<std::string, DriverError> source_result = ReadSourceFile(file_path);
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
}
