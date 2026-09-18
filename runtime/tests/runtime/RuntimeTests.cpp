#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"
#include "support/TempDir.h"

#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

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

	std::string ReadFileContents(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
	}

	const MidoriTest::ExecutedSnippet& RequireExecutedSnippet(const std::expected<MidoriTest::ExecutedSnippet, CompilerError>& run_result)
	{
		if (!run_result.has_value())
		{
			FAIL(std::string(run_result.error().Rendered()));
		}

		return run_result.value();
	}
}

TEST_CASE("VM executes file-backed runtime behavior inside a temporary directory", "[runtime][vm][filesystem]")
{
	const std::filesystem::path io_module_path = RepositoryRoot() / "MarmotPrelude" / "IO.mmt";
	const std::filesystem::path result_module_path = RepositoryRoot() / "MarmotPrelude" / "Prelude" / "Result.mmt";
	const MidoriTest::TempDir temp_dir("marmot-runtime-files");
	const std::filesystem::path data_file_path = temp_dir.Path() / "state.txt";
	const std::filesystem::path source_file_path = temp_dir.Path() / "RuntimeFileIO.mmt";

	const std::string source_code = std::format(
		R"(module RuntimeFileIO
import {{ "{}", "{}" }}
use IO.{{IOError}}
use Result.{{Result}}
def DescribeWrite = fn(result: Result<Unit, IOError>) -> Text => {{
    match result with
        case Result::Ok(_) => "wrote"
        case Result::Err(_) => "failed"
}};
def DescribeRead = fn(result: Result<Text, IOError>) -> Text => {{
    match result with
        case Result::Ok(value) => value
        case Result::Err(_) => "unreadable"
}};
def wrote = IO::TryWriteFile("{}", "alpha");
def exists = IO::FileExists("{}");
def contents = IO::TryReadFile("{}");
IO::PrintLine(DescribeWrite(wrote));
IO::PrintLine(if exists then "exists" else "missing");
IO::PrintLine(DescribeRead(contents));
def main = fn() -> Int => 0;
)",
		MidoriPathLiteral(io_module_path),
		MidoriPathLiteral(result_module_path),
		MidoriPathLiteral(data_file_path),
		MidoriPathLiteral(data_file_path),
		MidoriPathLiteral(data_file_path));

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout == "wrote\nexists\nalpha\n");
	REQUIRE(executed.m_output.m_stderr.empty());
	REQUIRE(std::filesystem::exists(data_file_path));
	REQUIRE(ReadFileContents(data_file_path) == "alpha");
}

TEST_CASE("VM captures stderr emitted by runtime code", "[runtime][vm][stderr]")
{
	const std::filesystem::path io_module_path = RepositoryRoot() / "MarmotPrelude" / "IO.mmt";

	const std::string source_code = std::format(
		R"(module RuntimeStderr
import {{ "{}" }}
IO::PrintErrorLine("panic on stderr");
def main = fn() -> Int => 0;
)",
		MidoriPathLiteral(io_module_path));

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, "RuntimeStderr.mmt");
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout.empty());
	REQUIRE(executed.m_output.m_stderr == "panic on stderr\n");
}

TEST_CASE("VM gives a closure the same result on repeated calls inside one scope", "[runtime][vm][closure]")
{
	const std::filesystem::path io_module_path = RepositoryRoot() / "MarmotPrelude" / "IO.mmt";

	const std::string source_code = std::format(
		R"(module RuntimeClosure
import {{ "{}" }}
def values = {{
	def y = 5;
	def f = fn(z : Int) -> Int => y + z;
	def first = f(2);
	def second = f(2);
	(first, second)
}};
match values with
	case (first, second) => {{
		IO::PrintLine(first as Text);
		IO::PrintLine(second as Text)
	}}
;
def main = fn() -> Int => 0;
)",
		MidoriPathLiteral(io_module_path));

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, "RuntimeClosure.mmt");
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout == "7\n7\n");
	REQUIRE(executed.m_output.m_stderr.empty());
}

TEST_CASE("VM executes tuple destructuring and backward ranges in-process", "[runtime][vm][aggregate]")
{
	const std::filesystem::path io_module_path = RepositoryRoot() / "MarmotPrelude" / "IO.mmt";

	const std::string source_code = std::format(
		R"(module RuntimeAggregate
import {{ "{}" }}
def pair = (2, 3);
def tuple_sum = match pair with
	case (x, y) => x + y
;
def backward = 10..-2..0;
def SumFrom = fn(values : Array<Int>, index : Int) -> Int =>
	if index >= #values then 0 else values[index] + SumFrom(values, index + 1);
def total = SumFrom([i for i in backward], 0);
IO::PrintLine("tuple_sum=" ++ (tuple_sum as Text));
IO::PrintLine("range_sum=" ++ (total as Text));
def main = fn() -> Int => 0;
)",
		MidoriPathLiteral(io_module_path));

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, "RuntimeAggregate.mmt");
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == EXIT_SUCCESS);
	REQUIRE(executed.m_output.m_stdout == "tuple_sum=5\nrange_sum=30\n");
	REQUIRE(executed.m_output.m_stderr.empty());
}

TEST_CASE("VM reports deterministic runtime errors for invalid array access", "[runtime][vm][error]")
{
	const MidoriTest::TempDir temp_dir("marmot-runtime-error");
	const std::filesystem::path source_file_path = temp_dir.Path() / "RuntimeError.mmt";
	const std::string source_code =
		R"(module RuntimeError
def value = [1, 2][4];
def main = fn() -> Int => 0;
)";
	std::ofstream(source_file_path) << source_code;

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == 1);
	REQUIRE(executed.m_output.m_stderr.empty());
	REQUIRE(executed.m_output.m_stdout.find("error[IndexOutOfBounds]") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("RuntimeError.mmt:2") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("Index out of bounds at index: 4.") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("def value = [1, 2][4];") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("stack trace:") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("main") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("[module RuntimeError]") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("$main$") == std::string::npos);
}

TEST_CASE("VM collapses recursive frames for stack overflow diagnostics", "[runtime][vm][error][stack]")
{
	const MidoriTest::TempDir temp_dir("marmot-runtime-stack-overflow");
	const std::filesystem::path source_file_path = temp_dir.Path() / "RuntimeStackOverflow.mmt";
	const std::string source_code =
		R"(module RuntimeStackOverflow
def recurse = fn(n : Int) -> Int => recurse(n + 1) + 1;
def value = recurse(0);
def main = fn() -> Int => 0;
)";
	std::ofstream(source_file_path) << source_code;

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, source_file_path.string());
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == 2);
	REQUIRE(executed.m_output.m_stderr.empty());
	REQUIRE(executed.m_output.m_stdout.find("panic[StackOverflow]") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("Stack overflow - exceeded maximum call depth.") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("stack trace:") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("[module RuntimeStackOverflow]") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("def recurse = fn(n : Int) -> Int => recurse(n + 1) + 1;") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("recursive calls]") != std::string::npos);
}

TEST_CASE("VM renders runtime source context from embedded executable metadata", "[runtime][vm][error][embedded]")
{
	const std::string source_code =
		R"(module EmbeddedRuntimeError
def value = [1, 2][4];
def main = fn() -> Int => 0;
)";

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, "EmbeddedRuntimeError.mmt");
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == 1);
	REQUIRE(executed.m_output.m_stdout.find("def value = [1, 2][4];") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("EmbeddedRuntimeError.mmt:2") != std::string::npos);
}

TEST_CASE("VM reports division by zero as a structured runtime error", "[runtime][vm][error][division]")
{
	const std::string source_code =
		R"(module RuntimeDivision
def value = 10 / 0;
def main = fn() -> Int => 0;
)";

	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> run_result =
		MidoriTest::ExecuteSnippet(source_code, "RuntimeDivision.mmt");
	const MidoriTest::ExecutedSnippet& executed = RequireExecutedSnippet(run_result);

	REQUIRE(executed.m_exit_code == 1);
	REQUIRE(executed.m_output.m_stdout.find("error[DivisionByZero]") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("Division by zero.") != std::string::npos);
	REQUIRE(executed.m_output.m_stdout.find("stack trace:") != std::string::npos);
}
