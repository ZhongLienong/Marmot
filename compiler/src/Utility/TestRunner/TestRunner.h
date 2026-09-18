#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Compiler/Result/Result.h"
#include "Utility/Project/ProjectManifest.h"

namespace MidoriTestRunner
{
	struct Options
	{
		std::filesystem::path m_start_path = ".";
		std::optional<std::string> m_filter = std::nullopt;
		std::optional<std::string> m_pattern = std::nullopt;
		std::optional<std::string> m_test_file = std::nullopt;
	};

	struct TestResult
	{
		std::string m_name;
		std::filesystem::path m_path;
		bool m_passed = false;
		bool m_expected_to_fail = false;
		bool m_timed_out = false;
		std::string m_output;
		int m_exit_code = 0;
		std::optional<std::string> m_error = std::nullopt;
		double m_duration_ms = 0.0;
		MidoriResult::CompilerReport m_report;
		std::string m_report_json;
	};

	struct WorkerOptions
	{
		std::filesystem::path m_test_path;
		std::filesystem::path m_result_directory;
		std::filesystem::path m_test_directory;
	};

	struct RunResult
	{
		std::filesystem::path m_root;
		std::filesystem::path m_test_directory;
		int m_timeout_ms = 30000;
		std::vector<TestResult> m_results;

		[[nodiscard]] int TotalCount() const;
		[[nodiscard]] int PassedCount() const;
		[[nodiscard]] int FailedCount() const;
		[[nodiscard]] double TotalDurationMs() const;
		[[nodiscard]] bool Succeeded() const;
		[[nodiscard]] std::string Rendered() const;
		[[nodiscard]] std::string MachineReadableJson() const;
	};

	[[nodiscard]] RunResult Run(const Options& options);

	[[nodiscard]] int RunWorker(const WorkerOptions& options);
}
