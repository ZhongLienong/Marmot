#include <catch2/catch_test_macros.hpp>

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/ImportResolver/ImportResolver.h"
#include "Utility/Driver/MarmotDriver.h"
#include "support/ScopedEnvVar.h"
#include "support/TempProject.h"
#include "Support/Optional.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
#ifdef _WIN32
	constexpr char s_search_path_separator = ';';
#else
	constexpr char s_search_path_separator = ':';
#endif
}

TEST_CASE("ImportResolver resolves relative path imports against the current file", "[module][import]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("src/Main.mmt", "module Main\n"),
		MarmotTest::TempProjectFile("src/lib/Helper.mmt", "module Helper\n")
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("src/Main.mmt"));
	const std::filesystem::path helper_file_path = std::filesystem::weakly_canonical(project.Path("src/lib/Helper.mmt"));

	const ImportResolver resolver(main_file_path.string(), {});
	const MarmotCompiler::Optional<std::string> resolved_import = resolver.ResolvePathImport("./lib/Helper.mmt");

	REQUIRE(resolved_import.HasValue());
	CHECK(resolved_import.Value() == helper_file_path.string());
}

TEST_CASE("ImportResolver resolves dotted system imports from explicit search paths", "[module][import]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Main.mmt", "module Main\n"),
		MarmotTest::TempProjectFile("stdlib/Std/IO.mmt", "module Std.IO\n")
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::filesystem::path io_module_path = std::filesystem::weakly_canonical(project.Path("stdlib/Std/IO.mmt"));

	const ImportResolver resolver(main_file_path.string(), { project.Path("stdlib") });
	const MarmotCompiler::Optional<std::string> resolved_import = resolver.ResolveSystemImport("Std.IO");

	REQUIRE(resolved_import.HasValue());
	CHECK(resolved_import.Value() == io_module_path.string());
}

TEST_CASE("MARMOT_PATH entries become canonical search paths and missing directories are dropped", "[module][import]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Main.mmt", "module Main\n"),
		MarmotTest::TempProjectFile("stdlib/Std/Math.mmt", "module Std.Math\n")
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::filesystem::path stdlib_path = std::filesystem::weakly_canonical(project.Path("stdlib"));
	const std::filesystem::path math_module_path = std::filesystem::weakly_canonical(project.Path("stdlib/Std/Math.mmt"));
	const std::string marmot_path_value = project.Path("missing").string() + std::string(1, s_search_path_separator) + project.Path("stdlib").string();
	const MarmotTest::ScopedEnvVar marmot_path("MARMOT_PATH", marmot_path_value);

	const CompilationInputs inputs = MarmotDriver::EnvironmentCompilationInputs();

	REQUIRE(inputs.SearchPaths().size() == 1);
	CHECK(inputs.SearchPaths().front() == stdlib_path);

	const ImportResolver resolver(main_file_path.string(), inputs.SearchPaths());
	const MarmotCompiler::Optional<std::string> resolved_import = resolver.ResolveSystemImport("Std.Math");
	REQUIRE(resolved_import.HasValue());
	CHECK(resolved_import.Value() == math_module_path.string());
}

TEST_CASE("The compiler's inputs ignore MARMOT_PATH unless the caller reads it", "[module][import]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("stdlib/Std/Math.mmt", "module Std.Math\n")
	});
	const MarmotTest::ScopedEnvVar marmot_path("MARMOT_PATH", project.Path("stdlib").string());

	CHECK(CompilationInputs().SearchPaths().empty());
}

TEST_CASE("ImportResolver returns nullopt for missing imports", "[module][import]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Main.mmt", "module Main\n")
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));

	const ImportResolver resolver(main_file_path.string(), { project.Root() / "stdlib" });

	CHECK_FALSE(resolver.ResolvePathImport("./Missing.mmt").HasValue());
	CHECK_FALSE(resolver.ResolveSystemImport("Std.Missing").HasValue());
}
