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
	using LoadArtifactResult = std::expected<MidoriExecutable, DriverError>;
	using RunResult = std::expected<int, RuntimeError>;
	using DriverResult = std::expected<int, DriverError>;

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
	[[nodiscard]] LoadArtifactResult LoadArtifact(const std::filesystem::path& path);
	// The directories in MARMOT_LIBRARY_PATH, in order.
	[[nodiscard]] std::vector<std::filesystem::path> EnvironmentLibraryPaths();
	// Loads every native library the executable names (`foreign ... from
	// "library"`), before it runs. Each is looked for at its configured path,
	// then in each search path, then beside the modules that declared it (in
	// lib/<platform>/, then the directory itself). A library that is missing,
	// fails to load, or lacks a symbol is an error. Loading runs a library's own
	// code, so only commands that run the program call this.
	[[nodiscard]] std::expected<void, DriverError> LoadNativeLibraries(const MidoriExecutable& executable, const NativeLibraryOptions& options);
	[[nodiscard]] RunResult RunExecutable(MidoriExecutable&& executable);
	[[nodiscard]] DriverResult CompileAndRunFile(const std::filesystem::path& file_path);
}
