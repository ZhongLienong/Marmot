#include "Utility/Driver/MidoriDriver.h"

#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Compiler/Compiler.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Interpreter/Worker/Worker.h"
#include "Utility/Project/ProjectManifest.h"

namespace
{
	[[nodiscard]] std::filesystem::path ResolveManifestInputPath(const std::filesystem::path& file_path)
	{
		std::error_code path_error;
		std::filesystem::path resolved_path = std::filesystem::absolute(file_path, path_error);
		if (path_error)
		{
			return file_path;
		}

		return resolved_path;
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
		return Compiler(std::move(source_code), std::move(file_name)).CompileWithReport();
	}

	CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path)
	{
		const std::filesystem::path manifest_input_path = ResolveManifestInputPath(file_path);
		MidoriProject::ApplyProjectManifestToEnvironment(manifest_input_path);

		SourceReadResult source_result = ReadSourceFile(file_path);
		if (!source_result.has_value())
		{
			return std::unexpected(std::move(source_result.error()));
		}

		MidoriResult::CompilationResult compile_result = CompileSourceWithReport(std::move(source_result.value()), file_path.string());
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

		RunResult run_result = RunExecutable(std::move(compiled_program).TakeExecutable());
		if (!run_result.has_value())
		{
			return std::unexpected(DriverError::Diagnostics(MidoriResult::CompilerDiagnostics(run_result.error().ToCompilerError())));
		}

		return run_result.value();
	}
}
