#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Utility/Driver/MarmotDriver.h"
#include "support/CompileHelpers.h"
#include "support/TempProject.h"

TEST_CASE("Sibling modules specialize shared generic bodies with nested lambdas", "[compiler][module][generic][publication]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Shared.mmt", R"(module Shared
public export { Make }
def Make = fn<T>(value: T) -> T => {
	def Wrap = fn<U>(item: U) -> (U, T) => (item, value);
	def (tag, result) = Wrap(0);
	if tag == 0 then result else value
};
)"),
		MarmotTest::TempProjectFile("Left.mmt", R"(module Left
import { "Shared.mmt" }
public export { Value }
def Value = fn() -> Int => Shared::Make(17);
)"),
		MarmotTest::TempProjectFile("Right.mmt", R"(module Right
import { "Shared.mmt" }
public export { Value }
def Value = fn() -> Text => Shared::Make("shared");
)"),
		MarmotTest::TempProjectFile("Main.mmt", R"(module Main
import { "Left.mmt", "Right.mmt" }
foreign "MARMOT_FFI_Print" Print: fn(Text) -> Unit from "marmotstdlib";
Print((Left::Value() as Text) ++ ":" ++ Right::Value() ++ "\n");
)")
	});
	std::optional<std::string> expected_bytecode;
	std::vector<std::string> expected_ir;
	std::vector<std::string> expected_ast;
	for (size_t jobs : { 1u, 2u, 4u, 4u })
	{
		CAPTURE(jobs);
		MarmotDriver::CompileFileWithReportResult compiled = MarmotDriver::CompileFileWithReport(project.Path("Main.mmt"), CompilationInputs().WithJobs(jobs).WithEmitMarmotIR(true).WithEmitAst(true));
		if (!compiled.has_value())
		{
			FAIL(compiled.error().Rendered());
		}
		std::ostringstream artifact;
		REQUIRE(MarmotBinaryArtifact::WriteExecutable(compiled->m_executable, artifact).has_value());
		if (!expected_bytecode.has_value())
		{
			expected_bytecode = artifact.str();
			expected_ir = compiled->m_marmot_ir;
			expected_ast = compiled->m_ast;
		}
		CHECK(artifact.str() == *expected_bytecode);
		CHECK(compiled->m_marmot_ir == expected_ir);
		CHECK(compiled->m_ast == expected_ast);
		const std::pair<int, MarmotTest::CapturedOutput> executed = MarmotTest::RunExecutable(compiled->m_executable);
		CHECK(executed.first == 0);
		CHECK(executed.second.m_stdout == "17:shared\n");
		CHECK(executed.second.m_stderr.empty());
	}
}
