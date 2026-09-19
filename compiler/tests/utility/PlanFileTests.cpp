#include <catch2/catch_test_macros.hpp>

#include "Utility/BuildPlan/BuildPlan.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
	MidoriTest::TempProject PlanProject()
	{
		return MidoriTest::TempProject
		({
			MidoriTest::TempProjectFile("src/Main.mmt", "module Main\nimport { \"<Greeting>\" }\ndef main = fn() -> Int => Greeting::Answer();\n"),
			MidoriTest::TempProjectFile("lib/Greeting.mmt", "module Greeting\npublic export { Answer }\ndef Answer = fn() -> Int => 42;\n"),
			MidoriTest::TempProjectFile("native/Native.mmt", "module Native\n"),
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
		"native_packages": [
			{
				"name": "Native",
				"root": "native",
				"library": "native/native.dll",
				"functions": { "MIDORI_FFI_Native_Answer": "native_answer" },
				"thread_safe": true,
				"checksum": "sha256:00"
			}
		]
	})", project.Root());
	REQUIRE(plan.has_value());

	CHECK(plan->m_entry == std::optional<std::filesystem::path>(project.Path("src/Main.mmt")));
	REQUIRE(plan->m_inputs.SearchPaths().size() == 1u);
	CHECK(plan->m_inputs.SearchPaths().front() == std::filesystem::weakly_canonical(project.Path("lib")));

	const std::optional<NativePackage> package = plan->m_inputs.FindNativePackage(project.Path("native"));
	REQUIRE(package.has_value());
	CHECK(package->m_name == "Native");
	CHECK(package->m_library == project.Path("native/native.dll"));
	CHECK(package->m_functions.at("MIDORI_FFI_Native_Answer") == "native_answer");
	CHECK(package->m_thread_safe);
	CHECK(package->m_checksum == std::optional<std::string>("sha256:00"));
	CHECK_FALSE(plan->m_inputs.FindNativePackage(project.Path("lib")).has_value());
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
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "native_packages": [{"name": "N", "root": "native", "library": "n.dll"}]})", base)
		== "native_packages[0]: missing \"functions\"");
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "native_packages": [{"name": "N", "root": "native", "library": "n.dll", "functions": {}, "threadsafe": true}]})", base)
		== "native_packages[0]: unknown member \"threadsafe\"");
	CHECK(CheckError(R"({"version": 1, "entry": "src/Main.mmt", "native_packages": [
		{"name": "N", "root": "native", "library": "n.dll", "functions": {}},
		{"name": "N", "root": "lib", "library": "m.dll", "functions": {}}]})", base)
		== "native_packages[1]: a second package named \"N\"");
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
	const MidoriDriver::RunResult run_result = MidoriDriver::RunExecutable(std::move(program).TakeExecutable());
	CHECK(run_result.has_value());
}
