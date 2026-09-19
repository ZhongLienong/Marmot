#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Result/Result.h"

namespace MidoriTestRunner
{
	struct Options
	{
		std::filesystem::path m_root;
		std::filesystem::path m_test_directory;
		int m_timeout_ms = 30000;
		// A build plan's inputs, for every test; without one, MARMOT_PATH.
		std::optional<std::filesystem::path> m_plan_file = std::nullopt;
		std::optional<CompilationInputs> m_inputs = std::nullopt;
		std::optional<std::string> m_filter = std::nullopt;
		std::optional<std::string> m_pattern = std::nullopt;
		std::optional<std::string> m_test_file = std::nullopt;

		// The test directory defaults to root/test (a relative one is under the
		// root), the timeout to 30000 ms. A plan that does not read is an error.
		[[nodiscard]] static std::expected<Options, std::string> Create(
			const std::filesystem::path& root,
			const std::optional<std::filesystem::path>& test_directory,
			const std::optional<int>& timeout_ms,
			const std::optional<std::filesystem::path>& plan_file);
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
		std::optional<std::filesystem::path> m_plan_file = std::nullopt;
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
