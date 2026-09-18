#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"
#include "support/TempDir.h"

#include <chrono>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <string>

namespace
{
	std::filesystem::path RepositoryRoot()
	{
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
	}

	std::string MidoriPathLiteral(const std::filesystem::path& path)
	{
		std::error_code error_code;
		const std::filesystem::path canonical_path = std::filesystem::weakly_canonical(path, error_code);
		if (!error_code)
		{
			return canonical_path.generic_string();
		}

		return path.lexically_normal().generic_string();
	}

	// The imports a program joining a worker needs: `join` evaluates to
	// Result<T, WorkerError>, and the program prints what it received.
	std::string JoinImports()
	{
		const std::filesystem::path prelude = RepositoryRoot() / "MarmotPrelude";
		return std::format(
			R"(import {{ "{}", "{}", "{}", "{}" }})",
			MidoriPathLiteral(prelude / "System.mmt"),
			MidoriPathLiteral(prelude / "IO.mmt"),
			MidoriPathLiteral(prelude / "Concurrency.mmt"),
			MidoriPathLiteral(prelude / "Prelude" / "Result.mmt"));
	}

	// Prints "join: cancelled", "join: failed: <message>" or "join: ok <value>".
	constexpr const char* DESCRIBE_JOIN = R"(def Describe = fn(result: Result<Int, WorkerError>) -> Text =>
    match result with
    case Result::Ok(value) => "ok " ++ (value as Text)
    case Result::Err(error) =>
        match error with
        case WorkerError::Cancelled() => "cancelled"
        case WorkerError::Failed(message) => "failed: " ++ message;
IO::PrintLine("join: " ++ Describe(r));
)";

	const MidoriTest::ExecutedSnippet& RequireExecutedSnippet(const std::expected<MidoriTest::ExecutedSnippet, CompilerError>& run_result)
	{
		if (!run_result.has_value())
		{
			FAIL(std::string(run_result.error().Rendered()));
		}

		return run_result.value();
	}
}

TEST_CASE("Joining a cancelled spinning worker yields a cancellation error value", "[runtime][worker][cancel]")
{
	const MidoriTest::TempDir temp_dir("marmot-worker-cancel");
	const std::filesystem::path source_file_path = temp_dir.Path() / "WorkerCancelJoin.mmt";

	const std::string source_code = std::format(
		R"(module WorkerCancelJoin
{}
def SpinFrom = fn(i: Int) -> Int => SpinFrom(i + 1);
def Spin = fn(dummy: Int) -> Int => SpinFrom(0);
def w = Concurrency::Spawn(0, Spin);
System::Sleep(50);
def cancelled = Concurrency::Cancel(w);
def r = Concurrency::Join(w);
{}IO::PrintLine("joiner continued");
)",
		JoinImports(), DESCRIBE_JOIN);

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	CAPTURE(executed.m_output.m_stdout);
	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout.find("join: cancelled") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("joiner continued") != std::string::npos);
}

TEST_CASE("Joining a cancelled tail-recursive worker yields a cancellation error value", "[runtime][worker][cancel]")
{
	const MidoriTest::TempDir temp_dir("marmot-worker-cancel-tail");
	const std::filesystem::path source_file_path = temp_dir.Path() / "WorkerCancelTailJoin.mmt";

	const std::string source_code = std::format(
		R"(module WorkerCancelTailJoin
{}
def SpinTail = fn(i: Int) -> Int => SpinTail(i + 1);
def w = Concurrency::Spawn(0, SpinTail);
System::Sleep(50);
def cancelled = Concurrency::Cancel(w);
def r = Concurrency::Join(w);
{})",
		JoinImports(), DESCRIBE_JOIN);

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	CAPTURE(executed.m_output.m_stdout);
	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout.find("join: cancelled") != std::string::npos);
}

TEST_CASE("Joining a cancelled worker reports cancellation, not a generic failure", "[runtime][worker][cancel]")
{
	const MidoriTest::TempDir temp_dir("marmot-worker-cancel-code");
	const std::filesystem::path source_file_path = temp_dir.Path() / "WorkerCancelCode.mmt";

	// The worker's WorkerCancelled code must map to WorkerError::Cancelled, not be
	// flattened into Failed with some internal message.
	const std::string source_code = std::format(
		R"(module WorkerCancelCode
{}
def SpinFrom = fn(i: Int) -> Int => SpinFrom(i + 1);
def Spin = fn(_dummy: Int) -> Int => SpinFrom(0);
def w = Concurrency::Spawn(0, Spin);
System::Sleep(50);
def cancelled = Concurrency::Cancel(w);
def r = Concurrency::Join(w);
{})",
		JoinImports(), DESCRIBE_JOIN);

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	CAPTURE(executed.m_output.m_stdout);
	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout.find("join: cancelled") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("failed") == std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("InternalTypeError") == std::string::npos);
}

TEST_CASE("Cancelling a worker blocked in a sleep wakes it promptly", "[runtime][worker][cancel]")
{
	const MidoriTest::TempDir temp_dir("marmot-worker-cancel-sleep");
	const std::filesystem::path source_file_path = temp_dir.Path() / "WorkerCancelSleep.mmt";

	const std::string source_code = std::format(
		R"(module WorkerCancelSleep
{}
def SleepLong = fn(_dummy: Int) -> Int => {{
    System::Sleep(30000);
    0
}};
def w = Concurrency::Spawn(0, SleepLong);
System::Sleep(300);
def cancelled = Concurrency::Cancel(w);
def r = Concurrency::Join(w);
{})",
		JoinImports(), DESCRIBE_JOIN);

	const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const std::chrono::steady_clock::duration elapsed = std::chrono::steady_clock::now() - start_time;
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	CAPTURE(executed.m_output.m_stdout);
	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout.find("join: cancelled") != std::string::npos);
	REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 10);
}
