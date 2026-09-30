#include <catch2/catch_test_macros.hpp>

#include "VmBytecode/Artifact/BinaryArtifact.h"
#include "Loader/ProgramLoader.h"
#include "support/OutputCapture.h"

#include <array>
#include <filesystem>
#include <sstream>

namespace
{
	std::filesystem::path Fixture(std::string_view name)
	{
		return std::filesystem::path(MARMOT_MMC_FIXTURES) / (std::string(name) + ".mmc");
	}
}

TEST_CASE("The VM independently loads the frozen version 13 artifacts", "[bytecode-artifact]")
{
	for (std::string_view name : { "minimal", "control", "embedded_error", "native" })
	{
		INFO(name);
		const std::expected<VmExecutable, std::string> loaded = VmBinaryArtifact::ReadExecutableFromFile(Fixture(name));
		REQUIRE(loaded.has_value());
		REQUIRE(loaded->GetProcedureCount() > 0);
		CHECK(loaded->m_procedure_names.size() == static_cast<size_t>(loaded->GetProcedureCount()));
		for (int procedure = 0; procedure < loaded->GetProcedureCount(); procedure += 1)
		{
			CHECK_FALSE(loaded->m_procedure_names[static_cast<size_t>(procedure)].empty());
			CHECK(loaded->GetByteCodeSize(procedure) > 0);
		}
		CHECK(loaded->GetLine(0, 0) == 0);
	}
}

TEST_CASE("The reader preserves embedded source metadata and native imports", "[bytecode-artifact]")
{
	const std::expected<VmExecutable, std::string> embedded = VmBinaryArtifact::ReadExecutableFromFile(Fixture("embedded_error"));
	const std::expected<VmExecutable, std::string> minimal = VmBinaryArtifact::ReadExecutableFromFile(Fixture("minimal"));
	const std::expected<VmExecutable, std::string> native = VmBinaryArtifact::ReadExecutableFromFile(Fixture("native"));
	REQUIRE(embedded.has_value());
	REQUIRE(minimal.has_value());
	REQUIRE(native.has_value());
	CHECK(embedded->FindSourceLines(embedded->GetProcedureSourcePath(0)) != nullptr);
	CHECK(minimal->FindSourceLines(minimal->GetProcedureSourcePath(0)) == nullptr);
	REQUIRE(native->GetNativeLibraries().size() == 1u);
	CHECK(native->GetNativeLibraries()[0u].m_name == "marmot_test_native");
	CHECK(native->GetNativeLibraries()[0u].m_symbols == std::vector<std::string>{ "marmot_test_answer" });
}

TEST_CASE("The reader rejects incompatible and corrupted artifacts", "[bytecode-artifact]")
{
	const std::array<std::pair<std::string_view, std::string_view>, 3u> invalid = {{
		{ "truncated", "Truncated artifact" },
		{ "checksum", "CRC32 mismatch" },
		{ "old_version", "format version mismatch: expected 13, got 11" }
	}};
	for (const std::pair<std::string_view, std::string_view>& fixture : invalid)
	{
		const std::expected<VmExecutable, std::string> loaded = VmBinaryArtifact::ReadExecutableFromFile(Fixture(fixture.first));
		REQUIRE_FALSE(loaded.has_value());
		CHECK(loaded.error().find(fixture.second) != std::string::npos);
	}
	std::istringstream invalid_magic("NOTMBC");
	const std::expected<VmExecutable, std::string> bad_magic = VmBinaryArtifact::ReadExecutable(invalid_magic);
	REQUIRE_FALSE(bad_magic.has_value());
	CHECK(bad_magic.error().find("magic") != std::string::npos);
	CHECK_FALSE(VmBinaryArtifact::ReadExecutableFromFile(Fixture("missing")).has_value());
}

TEST_CASE("The VM runs artifacts with and without embedded sources", "[bytecode-artifact][runtime]")
{

	std::expected<VmExecutable, std::string> control = VmBinaryArtifact::ReadExecutableFromFile(Fixture("control"));
	REQUIRE(control.has_value());
	MidoriTest::OutputCapture capture;
	const std::expected<int, RuntimeError> executed = MidoriProgramLoader::Run(std::move(control).value());
	const MidoriTest::CapturedOutput output = capture.Stop();
	REQUIRE(executed.has_value());
	CHECK(executed.value() == 0);
	CHECK(output.m_stdout == "large\n");
	std::expected<VmExecutable, std::string> embedded = VmBinaryArtifact::ReadExecutableFromFile(Fixture("embedded_error"));
	REQUIRE(embedded.has_value());
	const std::expected<int, RuntimeError> failed = MidoriProgramLoader::Run(std::move(embedded).value());
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().Rendered().find("def missing = values[4];") != std::string::npos);
}
