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
		MidoriResult::CompilerReport m_report;
		bool m_is_compilation_failure = false;

		static DriverError Compilation(MidoriResult::CompilerReport report);

		[[nodiscard]] std::string Rendered() const;
	};

	using CompileFileWithReportResult = std::expected<MidoriResult::CompiledProgram, DriverError>;

	[[nodiscard]] bool ShouldEmitMachineReadableWarnings();
	// What marmotc compiles with when it is given a file and no build plan:
	// `<Name>` imports through MARMOT_PATH, and no native packages.
	[[nodiscard]] CompilationInputs EnvironmentCompilationInputs();
	// Compiles with EnvironmentCompilationInputs().
	[[nodiscard]] MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name);
	[[nodiscard]] MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name, CompilationInputs inputs);
	// Compiles with EnvironmentCompilationInputs().
	[[nodiscard]] CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path);
	// Compiles with exactly `inputs` (a build plan's).
	[[nodiscard]] CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path, CompilationInputs inputs);
}
