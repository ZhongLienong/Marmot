#include <catch2/catch_test_macros.hpp>

#include "Utility/BuildPlan/BuildPlan.h"
#include "Utility/Driver/MarmotDriver.h"
#include "support/ScopedEnvVar.h"
#include "support/CompileHelpers.h"
#include "support/TempProject.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
	MarmotTest::TempProject PlanProject()
	{
		return MarmotTest::TempProject
		({
			MarmotTest::TempProjectFile("src/Main.mmt", "module Main\nimport { \"<Greeting>\" }\ndef main = fn() -> Int => Greeting::Answer();\n"),
			MarmotTest::TempProjectFile("lib/Greeting.mmt", "module Greeting\npublic export { Answer }\ndef Answer = fn() -> Int => 42;\n"),
			MarmotTest::TempProjectFile("native/bin/native.dll", "not a library\n"),
		});
	}

	std::string CheckError(std::string_view json, const std::filesystem::path& base)
	{
		const std::expected<BuildPlan, std::string> plan = MarmotBuildPlan::Parse(json, base);
		REQUIRE_FALSE(plan.has_value());
		return plan.error();
	}
}

TEST_CASE("A program compiled from a plan sees only the plan's search paths", "[plan]")
{
	const MarmotTest::TempProject project = PlanProject();
	// MARMOT_PATH points somewhere that would satisfy the import; the plan must
	// not read it.
	const MarmotTest::ScopedEnvVar marmot_path("MARMOT_PATH", project.Path("lib").string());

	const std::expected<BuildPlan, std::string> without_lib = MarmotBuildPlan::Parse(R"({"version": 1, "entry": "src/Main.mmt"})", project.Root());
	REQUIRE(without_lib.has_value());
	const MarmotDriver::CompileFileWithReportResult failed = MarmotDriver::CompileFileWithReport(without_lib->m_entry.value(), without_lib->m_inputs);
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().Rendered().find("Could not resolve import: <Greeting>") != std::string::npos);

	const std::expected<BuildPlan, std::string> with_lib = MarmotBuildPlan::Parse(R"({"version": 1, "entry": "src/Main.mmt", "search_paths": ["lib"]})", project.Root());
	REQUIRE(with_lib.has_value());
	MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(with_lib->m_entry.value(), with_lib->m_inputs);
	REQUIRE(compiled.has_value());

	MarmotResult::CompiledProgram program = std::move(compiled).value();
	const std::pair<int, MarmotTest::CapturedOutput> run_result = MarmotTest::RunExecutable(std::move(program).TakeExecutable());
	CHECK(run_result.first == 0);
}
