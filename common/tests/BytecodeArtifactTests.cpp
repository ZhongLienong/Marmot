#include <print>
#include <sstream>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"
#include "support/OutputCapture.h"

namespace
{
	[[nodiscard]] MidoriExecutable CompileOrFail(std::string source)
	{
		MidoriResult::CompilerResult result = MidoriTest::CompileSnippet(std::move(source));
		if (!result.has_value())
		{
			FAIL("Test snippet failed to compile.");
		}
		return std::move(result).value();
	}

	[[nodiscard]] std::string Serialize(const MidoriExecutable& executable, bool embed_sources = false)
	{
		std::ostringstream out;
		std::expected<void, std::string> write_result = MidoriBinaryArtifact::WriteExecutable(executable, out, embed_sources);
		if (!write_result.has_value())
		{
			FAIL(std::string("WriteExecutable failed: ") + write_result.error());
		}
		return out.str();
	}

	[[nodiscard]] MidoriExecutable Deserialize(const std::string& blob)
	{
		std::istringstream in(blob);
		std::expected<MidoriExecutable, std::string> read_result = MidoriBinaryArtifact::ReadExecutable(in);
		if (!read_result.has_value())
		{
			FAIL(std::string("ReadExecutable failed: ") + read_result.error());
		}
		return std::move(read_result).value();
	}

	[[nodiscard]] std::pair<int, std::string> RunArtifact(MidoriExecutable executable)
	{
		MidoriTest::OutputCapture capture;
		MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(executable));
		if (!run_result.has_value())
		{
			const RuntimeError& error = run_result.error();
			std::print("{}", error.Rendered());
			MidoriTest::CapturedOutput output = capture.Stop();
			return { error.ExitCode(), std::move(output.m_stdout) };
		}
		MidoriTest::CapturedOutput output = capture.Stop();
		return { run_result.value(), std::move(output.m_stdout) };
	}
}

TEST_CASE("BinaryArtifact round-trip produces byte-equal second serialization", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);
	const std::string first_blob = Serialize(executable);

	const MidoriExecutable reloaded = Deserialize(first_blob);
	const std::string second_blob = Serialize(reloaded);

	REQUIRE(first_blob == second_blob);
}

TEST_CASE("BinaryArtifact round-trip preserves procedure count and names", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def helper = fn() -> Int => 42;\n"
		"def main = fn() -> Int => helper();\n";

	const MidoriExecutable executable = CompileOrFail(source);
	const std::string blob = Serialize(executable);
	const MidoriExecutable reloaded = Deserialize(blob);

	REQUIRE(reloaded.GetProcedureCount() == executable.GetProcedureCount());
	for (int index = 0; index < executable.GetProcedureCount(); index += 1)
	{
		REQUIRE(reloaded.m_procedure_names[static_cast<size_t>(index)].c_str()
			== std::string(executable.m_procedure_names[static_cast<size_t>(index)].c_str()));
	}
}

TEST_CASE("BinaryArtifact round-trip preserves global count and string pool", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def greeting = \"hello\";\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);
	const std::string blob = Serialize(executable);
	const MidoriExecutable reloaded = Deserialize(blob);

	REQUIRE(reloaded.GetGlobalVariableCount() == executable.GetGlobalVariableCount());
	REQUIRE(reloaded.GetStringPool().size() == executable.GetStringPool().size());
}

TEST_CASE("BinaryArtifact round-trip preserves bytecode and line info", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);
	const std::string blob = Serialize(executable);
	const MidoriExecutable reloaded = Deserialize(blob);

	for (int proc = 0; proc < executable.GetProcedureCount(); proc += 1)
	{
		REQUIRE(reloaded.GetByteCodeSize(proc) == executable.GetByteCodeSize(proc));
		for (int instr = 0; instr < executable.GetByteCodeSize(proc); instr += 1)
		{
			REQUIRE(reloaded.ReadByteCode(instr, proc) == executable.ReadByteCode(instr, proc));
			REQUIRE(reloaded.GetLine(instr, proc) == executable.GetLine(instr, proc));
		}
	}
}

TEST_CASE("BinaryArtifact ReadExecutable rejects bad magic", "[bytecode-artifact]")
{
	const std::string bad_blob = "NOTMBC\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0";
	std::istringstream in(bad_blob, std::ios::binary);
	const std::expected<MidoriExecutable, std::string> result = MidoriBinaryArtifact::ReadExecutable(in);

	REQUIRE_FALSE(result.has_value());
	REQUIRE(result.error().find("magic") != std::string::npos);
}

TEST_CASE("BinaryArtifact ReadExecutable rejects version mismatch", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);
	std::string blob = Serialize(executable);

	// Corrupt the format_version field (bytes 4-7) to an invalid version
	if (blob.size() >= 8u)
	{
		blob[4] = static_cast<char>(0xFFu);
		blob[5] = static_cast<char>(0xFFu);
		blob[6] = static_cast<char>(0xFFu);
		blob[7] = static_cast<char>(0xFFu);
	}

	std::istringstream in(blob, std::ios::binary);
	const std::expected<MidoriExecutable, std::string> result = MidoriBinaryArtifact::ReadExecutable(in);

	REQUIRE_FALSE(result.has_value());
	REQUIRE(result.error().find("version") != std::string::npos);
}

TEST_CASE("BinaryArtifact ReadExecutable rejects corrupt payload (CRC mismatch)", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);
	std::string blob = Serialize(executable);

	// Flip a bit in the payload (past the 32-byte header)
	if (blob.size() > 33u)
	{
		blob[33] ^= 0x01u;
	}

	std::istringstream in(blob, std::ios::binary);
	const std::expected<MidoriExecutable, std::string> result = MidoriBinaryArtifact::ReadExecutable(in);

	REQUIRE_FALSE(result.has_value());
	REQUIRE(result.error().find("CRC32") != std::string::npos);
}

TEST_CASE("BinaryArtifact round-trip with embed-sources flag", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const MidoriExecutable executable = CompileOrFail(source);

	const std::string blob_with_sources = Serialize(executable, true);
	const std::string blob_without_sources = Serialize(executable, false);

	// Embedded-sources artifact is at least as large
	REQUIRE(blob_with_sources.size() >= blob_without_sources.size());

	// Both round-trip cleanly
	const MidoriExecutable reloaded_with = Deserialize(blob_with_sources);
	const MidoriExecutable reloaded_without = Deserialize(blob_without_sources);

	REQUIRE(reloaded_with.GetProcedureCount() == executable.GetProcedureCount());
	REQUIRE(reloaded_without.GetProcedureCount() == executable.GetProcedureCount());

	// Second-pass serialization (without embed) is byte-equal for both
	const std::string second_with = Serialize(reloaded_with);
	const std::string second_without = Serialize(reloaded_without);
	REQUIRE(second_with == second_without);
}

TEST_CASE("BinaryArtifact round-trip run produces identical exit code to direct execution", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => 0;\n";

	const auto [direct_exit, direct_stdout] = RunArtifact(CompileOrFail(source));
	const auto [artifact_exit, artifact_stdout] = RunArtifact(Deserialize(Serialize(CompileOrFail(source))));

	REQUIRE(direct_exit == EXIT_SUCCESS);
	REQUIRE(artifact_exit == direct_exit);
}

TEST_CASE("BinaryArtifact run without embedded sources degrades runtime error gracefully", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def value = [1, 2][4];\n"
		"def main = fn() -> Int => 0;\n";

	const std::string blob = Serialize(CompileOrFail(source), false);
	const auto [exit_code, stdout_output] = RunArtifact(Deserialize(blob));

	REQUIRE(exit_code == 1);
	REQUIRE(stdout_output.find("error[IndexOutOfBounds]") != std::string::npos);
	REQUIRE(stdout_output.find("def value = [1, 2][4];") == std::string::npos);
}

TEST_CASE("BinaryArtifact run with embedded sources preserves runtime error source context", "[bytecode-artifact]")
{
	const std::string source =
		"module Main\n"
		"\n"
		"def value = [1, 2][4];\n"
		"def main = fn() -> Int => 0;\n";

	const std::string blob = Serialize(CompileOrFail(source), true);
	const auto [exit_code, stdout_output] = RunArtifact(Deserialize(blob));

	REQUIRE(exit_code == 1);
	REQUIRE(stdout_output.find("error[IndexOutOfBounds]") != std::string::npos);
	REQUIRE(stdout_output.find("def value = [1, 2][4];") != std::string::npos);
}

TEST_CASE("BinaryArtifact ReadExecutableFromFile returns error for missing file", "[bytecode-artifact]")
{
	const std::expected<MidoriExecutable, std::string> result =
		MidoriBinaryArtifact::ReadExecutableFromFile("nonexistent_artifact_file.mmc");

	REQUIRE_FALSE(result.has_value());
	REQUIRE_FALSE(result.error().empty());
}
