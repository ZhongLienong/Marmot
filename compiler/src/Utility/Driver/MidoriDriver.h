#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Result/Result.h"
#include "Utility/BuildPlan/BuildPlan.h"

namespace MidoriDriver
{
	struct DriverError
	{
		std::string m_message;
		std::optional<MidoriResult::CompilerReport> m_report = std::nullopt;
		bool m_is_compilation_failure = false;

		static DriverError FileSystem(std::string message);
		static DriverError Compilation(MidoriResult::CompilerReport report);
		static DriverError Compilation(MidoriResult::CompilerDiagnostics diagnostics);
		static DriverError Diagnostics(MidoriResult::CompilerReport report);
		static DriverError Diagnostics(MidoriResult::CompilerDiagnostics diagnostics);

		[[nodiscard]] std::string Rendered() const;
	};

	using SourceReadResult = std::expected<std::string, DriverError>;
	using CompileFileWithReportResult = std::expected<MidoriResult::CompiledProgram, DriverError>;
	using CompileFileResult = std::expected<MidoriExecutable, DriverError>;

	[[nodiscard]] SourceReadResult ReadSourceFile(const std::filesystem::path& file_path);
	// The directories in MARMOT_PATH, in order.
	[[nodiscard]] std::vector<std::filesystem::path> EnvironmentSearchPaths();
	// What marmotc compiles with when it is given a file and no build plan:
	// `<Name>` imports through MARMOT_PATH, and no native packages.
	[[nodiscard]] CompilationInputs EnvironmentCompilationInputs();
	// Compiles with EnvironmentCompilationInputs().
	[[nodiscard]] MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name);
	[[nodiscard]] MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name, CompilationInputs inputs);
	[[nodiscard]] MidoriResult::CompilerResult CompileSource(std::string source_code, std::string file_name);
	// Compiles with EnvironmentCompilationInputs().
	[[nodiscard]] CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path);
	// Compiles with exactly `inputs` (a build plan's).
	[[nodiscard]] CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path, CompilationInputs inputs);
	[[nodiscard]] CompileFileResult CompileFile(const std::filesystem::path& file_path);
}
