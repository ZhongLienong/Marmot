#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

#include "Compiler/Result/Result.h"

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
	[[nodiscard]] MidoriResult::CompilationResult CompileSourceWithReport(std::string source_code, std::string file_name);
	[[nodiscard]] MidoriResult::CompilerResult CompileSource(std::string source_code, std::string file_name);
	[[nodiscard]] CompileFileWithReportResult CompileFileWithReport(const std::filesystem::path& file_path);
	[[nodiscard]] CompileFileResult CompileFile(const std::filesystem::path& file_path);
	[[nodiscard]] LoadArtifactResult LoadArtifact(const std::filesystem::path& path);
	[[nodiscard]] RunResult RunExecutable(MidoriExecutable&& executable);
	[[nodiscard]] DriverResult CompileAndRunFile(const std::filesystem::path& file_path);
}
