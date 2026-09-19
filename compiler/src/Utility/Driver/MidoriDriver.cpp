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

#include "Common/Environment/Environment.h"
#include "Compiler/Compiler.h"

namespace
{
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

	std::vector<std::filesystem::path> EnvironmentSearchPaths()
	{
		return MidoriEnvironment::ReadPathList("MARMOT_PATH");
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
}
