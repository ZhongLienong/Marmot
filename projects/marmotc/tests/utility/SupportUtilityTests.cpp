#include <catch2/catch_test_macros.hpp>

#include "support/OutputCapture.h"
#include "support/ScopedEnvVar.h"
#include "support/TempDir.h"
#include "Support/Optional.h"

#include <cstdio>
#include <print>
#include <string>

TEST_CASE("ScopedEnvVar restores the prior environment value", "[support][env]")
{
	const std::string env_name = "MARMOT_PHASE2_SUPPORT_ENV";
	const MarmotTest::ScopedEnvVar clear_guard(env_name, MarmotCompiler::None);

	{
		const MarmotTest::ScopedEnvVar value_guard(env_name, std::string("phase2"));
		REQUIRE(MarmotTest::ScopedEnvVar::Read(env_name) == MarmotCompiler::Optional<std::string>("phase2"));
	}

	REQUIRE_FALSE(MarmotTest::ScopedEnvVar::Read(env_name).HasValue());
}

TEST_CASE("TempDir creates files that are removed with the fixture", "[support][tempdir]")
{
	std::filesystem::path root_path;
	std::filesystem::path file_path;

	{
		const MarmotTest::TempDir temp_dir("marmot-support-tempdir");
		root_path = temp_dir.Path();
		file_path = temp_dir.WriteTextFile("nested/sample.txt", "contents");

		REQUIRE(std::filesystem::exists(root_path));
		REQUIRE(root_path == std::filesystem::weakly_canonical(root_path));
		REQUIRE(std::filesystem::exists(file_path));
	}

	REQUIRE_FALSE(std::filesystem::exists(root_path));
	REQUIRE_FALSE(std::filesystem::exists(file_path));
}

TEST_CASE("OutputCapture captures stdout and stderr from native utilities", "[support][output]")
{
	MarmotTest::OutputCapture capture;
	std::print("captured stdout");
	std::fputs("captured stderr", stderr);

	const MarmotTest::CapturedOutput output = capture.Stop();

	REQUIRE(output.m_stdout.find("captured stdout") != std::string::npos);
	REQUIRE(output.m_stderr.find("captured stderr") != std::string::npos);
}
