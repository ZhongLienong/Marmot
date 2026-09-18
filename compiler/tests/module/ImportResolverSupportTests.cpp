#include <catch2/catch_test_macros.hpp>

#include "Compiler/ImportResolver/ImportResolver.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"

TEST_CASE("TempProject and ScopedEnvVar make system import resolution deterministic", "[module][support]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\n"),
		MidoriTest::TempProjectFile("stdlib/Std/IO.mmt", "module Std.IO\n")
	});

	const std::filesystem::path main_file_path = project.Path("Main.mmt");
	const std::filesystem::path module_file_path = std::filesystem::weakly_canonical(project.Path("stdlib/Std/IO.mmt"));
	const MidoriTest::ScopedEnvVar marmot_path("MARMOT_PATH", project.Path("stdlib").string());

	const ImportResolver resolver(main_file_path.string());
	const std::optional<ImportResolver::ResolvedImport> resolved_import = resolver.Resolve("<Std.IO>");

	REQUIRE(resolved_import.has_value());
	REQUIRE(resolved_import->m_type == ImportResolver::ImportType::SYSTEM);
	REQUIRE(resolved_import->m_absolute_path == module_file_path.string());
}
