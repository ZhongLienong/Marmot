#include <catch2/catch_test_macros.hpp>

#include "Common/Printer/Printer.h"
#include "support/OutputCapture.h"
#include "support/ScopedEnvVar.h"
#include "support/TempDir.h"

#include <cstdio>
#include <optional>
#include <string>

TEST_CASE("ScopedEnvVar restores the prior environment value", "[support][env]")
{
	const std::string env_name = "MARMOT_PHASE2_SUPPORT_ENV";
	const MidoriTest::ScopedEnvVar clear_guard(env_name, std::nullopt);

	{
		const MidoriTest::ScopedEnvVar value_guard(env_name, std::string("phase2"));
		REQUIRE(MidoriTest::ScopedEnvVar::Read(env_name) == std::optional<std::string>("phase2"));
	}

	REQUIRE_FALSE(MidoriTest::ScopedEnvVar::Read(env_name).has_value());
}

TEST_CASE("TempDir creates files that are removed with the fixture", "[support][tempdir]")
{
	std::filesystem::path root_path;
	std::filesystem::path file_path;

	{
		const MidoriTest::TempDir temp_dir("marmot-support-tempdir");
		root_path = temp_dir.Path();
		file_path = temp_dir.WriteTextFile("nested/sample.txt", "contents");

		REQUIRE(std::filesystem::exists(root_path));
		REQUIRE(std::filesystem::exists(file_path));
	}

	REQUIRE_FALSE(std::filesystem::exists(root_path));
	REQUIRE_FALSE(std::filesystem::exists(file_path));
}

TEST_CASE("OutputCapture captures stdout and stderr from native utilities", "[support][output]")
{
	MidoriTest::OutputCapture capture;
	Printer::Print("captured stdout");
	std::fputs("captured stderr", stderr);

	const MidoriTest::CapturedOutput output = capture.Stop();

	REQUIRE(output.m_stdout.find("captured stdout") != std::string::npos);
	REQUIRE(output.m_stderr.find("captured stderr") != std::string::npos);
}
