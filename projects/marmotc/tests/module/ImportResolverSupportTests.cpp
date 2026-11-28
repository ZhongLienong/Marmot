#include <catch2/catch_test_macros.hpp>

#include "Compiler/ImportResolver/ImportResolver.h"
#include "Utility/Driver/MarmotDriver.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"
#include "Support/Optional.h"

TEST_CASE("TempProject and ScopedEnvVar make system import resolution deterministic", "[module][support]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Main.mmt", "module Main\n"),
		MarmotTest::TempProjectFile("stdlib/Std/IO.mmt", "module Std.IO\n")
	});

	const std::filesystem::path main_file_path = project.Path("Main.mmt");
	const std::filesystem::path module_file_path = std::filesystem::weakly_canonical(project.Path("stdlib/Std/IO.mmt"));
	const MarmotTest::ScopedEnvVar marmot_path("MARMOT_PATH", project.Path("stdlib").string());

	const ImportResolver resolver(main_file_path.string(), MarmotDriver::EnvironmentCompilationInputs().SearchPaths());
	const MarmotCompiler::Optional<std::string> resolved_import = resolver.ResolveSystemImport("Std.IO");

	REQUIRE(resolved_import.HasValue());
	REQUIRE(resolved_import.Value() == module_file_path.string());
}
