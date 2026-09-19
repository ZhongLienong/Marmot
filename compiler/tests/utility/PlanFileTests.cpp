#include <catch2/catch_test_macros.hpp>

#include "Utility/BuildPlan/BuildPlan.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"
#include "Loader/ProgramLoader.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
	MidoriTest::TempProject PlanProject()
	{
		return MidoriTest::TempProject
		({
			MidoriTest::TempProjectFile("src/Main.mmt", "module Main\nimport { \"<Greeting>\" }\ndef main = fn() -> Int => Greeting::Answer();\n"),
			MidoriTest::TempProjectFile("lib/Greeting.mmt", "module Greeting\npublic export { Answer }\ndef Answer = fn() -> Int => 42;\n"),
			MidoriTest::TempProjectFile("native/bin/native.dll", "not a library\n"),
		});
	}

	std::string CheckError(std::string_view json, const std::filesystem::path& base)
	{
		const std::expected<BuildPlan, std::string> plan = MidoriBuildPlan::Parse(json, base);
		REQUIRE_FALSE(plan.has_value());
		return plan.error();
	}
}

TEST_CASE("A build plan resolves its paths against the plan's directory", "[plan]")
{
	const MidoriTest::TempProject project = PlanProject();
	const std::expected<BuildPlan, std::string> plan = MidoriBuildPlan::Parse(R"({
		"version": 1,
		"entry": "src/Main.mmt",
		"search_paths": ["lib"],
		"native_libraries": [
			{ "name": "native", "thread_safe": true, "checksum": "sha256:00" },
			{ "name": "plain" }
		]
	})", project.Root());
	REQUIRE(plan.has_value());

	CHECK(plan->m_entry == std::optional<std::filesystem::path>(project.Path("src/Main.mmt")));
	REQUIRE(plan->m_inputs.SearchPaths().size() == 1u);
	CHECK(plan->m_inputs.SearchPaths().front() == std::filesystem::weakly_canonical(project.Path("lib")));

	REQUIRE(plan->m_inputs.NativeLibraryPolicies().size() == 2u);
	const NativeLibraryPolicy& native = plan->m_inputs.NativeLibraryPolicies().at("native");
	CHECK(native.m_thread_safe);
	CHECK(native.m_checksum == std::optional<std::string>("sha256:00"));
	const NativeLibraryPolicy& plain = plan->m_inputs.NativeLibraryPolicies().at("plain");
	CHECK_FALSE(plain.m_thread_safe);
	CHECK_FALSE(plain.m_checksum.has_value());
}

TEST_CASE("A build plan rejects what it does not understand", "[plan]")
{
	const MidoriTest::TempProject project = PlanProject();
	const std::filesystem::path base = project.Root();

	CHECK(CheckError(R"([])", base) == "the plan must be a JSON object, found array");
	CHECK(CheckError(R"({"entry": "src/Main.mmt"})", base) == "plan: missing \"version\"");
	CHECK(CheckError(R"({"version": 2, "entry": "src/Main.mmt"})", base) == "version: this compiler reads plan version 1; the plan says 2");
	const std::expected<BuildPlan, std::string> without_entry = MidoriBuildPlan::Parse(R"({"version": 1, "search_paths": ["lib"]})", base);
	REQUIRE(without_entry.has_value());
	CHECK_FALSE(without_entry->m_entry.has_value());
	CHECK(CheckError(R"({"version": 1, "entry": "src/Missing.mmt"})", base).starts_with("entry: no such file: "));
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "serach_paths": []})", base) == "plan: unknown member \"serach_paths\"");
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "search_paths": ["lib", 3]})", base) == "search_paths[1]: expected a string, found number");
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "search_paths": ["nowhere"]})", base).starts_with("search_paths[0]: not a directory: "));
	CHECK(CheckError(R"({"version": 1, "native_libraries": [{"checksum": "sha256:00"}]})", base) == "native_libraries[0]: missing \"name\"");
	// Where a library's file is belongs to the machine that runs the program.
	CHECK(CheckError(R"({"version": 1, "native_libraries": [{"name": "n", "path": "n.dll"}]})", base)
		== "native_libraries[0]: unknown member \"path\"");
	CHECK(CheckError(R"({"version": 1, "library_paths": ["lib"]})", base) == "plan: unknown member \"library_paths\"");
	CHECK(CheckError(R"({"version": 1, "native_libraries": [{"name": "n", "threadsafe": true}]})", base)
		== "native_libraries[0]: unknown member \"threadsafe\"");
	CHECK(CheckError(R"({"version": 1, "native_libraries": [{"name": "n"}, {"name": "n"}]})", base)
		== "native_libraries[1]: a second library named \"n\"");
	CHECK(CheckError(R"({"version": 1, "native_packages": []})", base) == "plan: unknown member \"native_packages\"");
	CHECK(CheckError(R"({"version": 1,)", base).starts_with("line 1, column "));
}

TEST_CASE("A program compiled from a plan sees only the plan's search paths", "[plan]")
{
	const MidoriTest::TempProject project = PlanProject();
	// MARMOT_PATH points somewhere that would satisfy the import; the plan must
	// not read it.
	const MidoriTest::ScopedEnvVar marmot_path("MARMOT_PATH", project.Path("lib").string());

	const std::expected<BuildPlan, std::string> without_lib = MidoriBuildPlan::Parse(R"({"version": 1, "entry": "src/Main.mmt"})", project.Root());
	REQUIRE(without_lib.has_value());
	const MidoriDriver::CompileFileWithReportResult failed = MidoriDriver::CompileFileWithReport(without_lib->m_entry.value(), without_lib->m_inputs);
	REQUIRE_FALSE(failed.has_value());
	CHECK(failed.error().Rendered().find("Could not resolve import: <Greeting>") != std::string::npos);

	const std::expected<BuildPlan, std::string> with_lib = MidoriBuildPlan::Parse(R"({"version": 1, "entry": "src/Main.mmt", "search_paths": ["lib"]})", project.Root());
	REQUIRE(with_lib.has_value());
	MidoriDriver::CompileFileWithReportResult compiled = MidoriDriver::CompileFileWithReport(with_lib->m_entry.value(), with_lib->m_inputs);
	REQUIRE(compiled.has_value());

	MidoriResult::CompiledProgram program = std::move(compiled).value();
	const std::expected<int, RuntimeError> run_result = MidoriProgramLoader::Run(std::move(program).TakeExecutable());
	CHECK(run_result.has_value());
}
