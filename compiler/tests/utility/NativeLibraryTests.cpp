#include <catch2/catch_test_macros.hpp>

#include "Common/BuildConfig/BuildConfig.h"
#include "Common/BytecodeArtifact/BinaryArtifact.h"
#include "Loader/ProgramLoader.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/OutputCapture.h"
#include "support/TempProject.h"

#include <expected>
#include <filesystem>
#include <format>
#include <sstream>
#include <string>
#include <vector>

// The library is loaded once per process (SharedLibraryCache), so each test
// that must see a fresh load uses its own copy under its own name.

namespace
{
	std::filesystem::path TestLibraryFile(std::string_view name)
	{
#if defined(_WIN32)
		return std::filesystem::path(MARMOT_TEST_NATIVE_DIR) / (std::string(name) + ".dll");
#elif defined(__APPLE__)
		return std::filesystem::path(MARMOT_TEST_NATIVE_DIR) / ("lib" + std::string(name) + ".dylib");
#else
		return std::filesystem::path(MARMOT_TEST_NATIVE_DIR) / ("lib" + std::string(name) + ".so");
#endif
	}

	std::filesystem::path PlatformFileName(std::string_view name)
	{
		return TestLibraryFile(name).filename();
	}

	// A program that prints "ok" when marmot_test_add(40, 2) is 42.
	std::string AddingProgram(std::string_view library)
	{
		return std::format(
			"module Main\n"
			"foreign \"MIDORI_FFI_Print\" Print : fn(Text) -> Unit;\n"
			"foreign \"{}\"\n"
			"{{\n"
			"\t\"marmot_test_add\" Add : fn(Int, Int) -> Int;\n"
			"\t\"marmot_test_answer\" Answer : fn() -> Int;\n"
			"}}\n"
			"if Add(40, 2) == Answer() then Print(\"ok\") else Print(\"wrong\");\n",
			library);
	}

	MidoriExecutable Compile(const MidoriTest::TempProject& project, std::string_view file, CompilationInputs inputs = CompilationInputs())
	{
		const MidoriBuild::ScopedTestModeOverride quiet(true);
		MidoriDriver::CompileFileWithReportResult compiled = MidoriDriver::CompileFileWithReport(project.Path(std::string(file)), std::move(inputs));
		REQUIRE(compiled.has_value());
		return std::move(compiled).value().TakeExecutable();
	}

	std::string Run(MidoriExecutable&& executable)
	{
		const MidoriBuild::ScopedTestModeOverride quiet(true);
		MidoriTest::OutputCapture capture;
		const MidoriDriver::RunResult result = MidoriProgramLoader::Run(std::move(executable));
		const std::string output = capture.Stop().m_stdout;
		REQUIRE(result.has_value());
		return output;
	}

	// A program that runs Answer() on a worker and prints what the join gave back.
	std::string WorkerProgram(std::string_view library)
	{
		const std::filesystem::path prelude = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() / "MarmotPrelude";
		return std::format(
			"module Main\n"
			"import {{ \"{}\", \"{}\", \"{}\" }}\n"
			"foreign \"marmot_test_answer\" Answer : fn() -> Int from \"{}\";\n"
			"def Work = fn(dummy: Int) -> Int => Answer();\n"
			"def r = Concurrency::Join(Concurrency::Spawn(0, Work));\n"
			"match r with\n"
			"case Result::Ok(value) => IO::PrintLine(\"ok \" ++ (value as Text))\n"
			"case Result::Err(error) =>\n"
			"    match error with\n"
			"    case WorkerError::Cancelled() => IO::PrintLine(\"cancelled\")\n"
			"    case WorkerError::Failed(message) => IO::PrintLine(message);\n",
			std::filesystem::weakly_canonical(prelude / "IO.mmt").generic_string(),
			std::filesystem::weakly_canonical(prelude / "Concurrency.mmt").generic_string(),
			std::filesystem::weakly_canonical(prelude / "Prelude" / "Result.mmt").generic_string(),
			library);
	}

	std::string LoadError(const MidoriExecutable& executable, const MidoriProgramLoader::NativeLibraryLocations& locations)
	{
		const std::expected<void, std::string> loaded = MidoriProgramLoader::LoadNativeLibraries(executable, locations);
		REQUIRE_FALSE(loaded.has_value());
		return loaded.error();
	}

	bool Loads(const MidoriExecutable& executable, const MidoriProgramLoader::NativeLibraryLocations& locations)
	{
		const std::expected<void, std::string> loaded = MidoriProgramLoader::LoadNativeLibraries(executable, locations);
		if (!loaded.has_value())
		{
			UNSCOPED_INFO(loaded.error());
		}
		return loaded.has_value();
	}
}

TEST_CASE("A foreign function from a native library runs when found through the library path", "[ffi][native]")
{
	const MidoriTest::TempProject project({ MidoriTest::TempProjectFile("Main.mmt", AddingProgram("marmot_test_native")) });
	MidoriExecutable executable = Compile(project, "Main.mmt");

	REQUIRE(executable.GetNativeLibraries().size() == 1u);
	CHECK(executable.GetNativeLibraries()[0u].m_name == "marmot_test_native");
	CHECK(executable.GetNativeLibraries()[0u].m_symbols == std::vector<std::string>{ "marmot_test_add", "marmot_test_answer" });

	REQUIRE(Loads(executable, MidoriProgramLoader::NativeLibraryLocations{ .m_search_paths = { MARMOT_TEST_NATIVE_DIR } }));
	CHECK(Run(std::move(executable)) == "ok");
}

TEST_CASE("A native library beside the declaring module is found without a library path", "[ffi][native]")
{
	const MidoriTest::TempProject project({ MidoriTest::TempProjectFile("pkg/Main.mmt", AddingProgram("marmot_test_native_beside")) });
#if defined(_WIN32)
	const std::filesystem::path platform_directory = std::filesystem::path("lib") / "windows" / "x64";
#elif defined(__APPLE__)
	const std::filesystem::path platform_directory = std::filesystem::path("lib") / "macos";
#else
	const std::filesystem::path platform_directory = std::filesystem::path("lib") / "linux" / "x86_64";
#endif
	std::filesystem::create_directories(project.Path("pkg") / platform_directory);
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), project.Path("pkg") / platform_directory / PlatformFileName("marmot_test_native_beside"));

	MidoriExecutable executable = Compile(project, "pkg/Main.mmt");
	REQUIRE(Loads(executable, MidoriProgramLoader::NativeLibraryLocations{}));
	CHECK(Run(std::move(executable)) == "ok");
}

TEST_CASE("A .mmc records its native libraries and runs from them", "[ffi][native][bytecode-artifact]")
{
	const MidoriTest::TempProject project({ MidoriTest::TempProjectFile("Main.mmt", AddingProgram("marmot_test_native_artifact")) });
	// Beside the module: the .mmc finds it through the module's directory.
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), project.Root() / PlatformFileName("marmot_test_native_artifact"));
	const MidoriExecutable executable = Compile(
		project,
		"Main.mmt",
		CompilationInputs().WithNativeLibraryPolicies({ { "marmot_test_native_artifact", NativeLibraryPolicy{ .m_thread_safe = true } } }));

	std::stringstream artifact;
	REQUIRE(MidoriBinaryArtifact::WriteExecutable(executable, artifact, false).has_value());
	std::expected<MidoriExecutable, std::string> reloaded = MidoriBinaryArtifact::ReadExecutable(artifact);
	REQUIRE(reloaded.has_value());

	REQUIRE(reloaded->GetNativeLibraries().size() == 1u);
	const NativeLibraryImport& library = reloaded->GetNativeLibraries()[0u];
	CHECK(library.m_name == "marmot_test_native_artifact");
	CHECK(library.m_symbols == executable.GetNativeLibraries()[0u].m_symbols);
	CHECK(library.m_hint_directories == executable.GetNativeLibraries()[0u].m_hint_directories);
	CHECK(library.m_policy.m_thread_safe);
	CHECK_FALSE(library.m_policy.m_checksum.has_value());

	REQUIRE(Loads(reloaded.value(), MidoriProgramLoader::NativeLibraryLocations{}));
	CHECK(Run(std::move(reloaded).value()) == "ok");
}

TEST_CASE("A native library that cannot be found or lacks a symbol stops the run", "[ffi][native]")
{
	const MidoriTest::TempProject missing({ MidoriTest::TempProjectFile("Main.mmt", AddingProgram("marmot_no_such_library")) });
	const std::string not_found = LoadError(Compile(missing, "Main.mmt"), MidoriProgramLoader::NativeLibraryLocations{ .m_search_paths = { MARMOT_TEST_NATIVE_DIR } });
	CHECK(not_found.find("native library 'marmot_no_such_library' not found") != std::string::npos);
	CHECK(not_found.find(PlatformFileName("marmot_no_such_library").string()) != std::string::npos);

	const MidoriTest::TempProject lacking({ MidoriTest::TempProjectFile(
		"Main.mmt",
		"module Main\n"
		"foreign \"marmot_test_nothing\" Nothing : fn() -> Int from \"marmot_test_native_lacking\";\n"
		"Nothing();\n") });
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), lacking.Root() / PlatformFileName("marmot_test_native_lacking"));
	const std::string no_symbol = LoadError(Compile(lacking, "Main.mmt"), MidoriProgramLoader::NativeLibraryLocations{});
	CHECK(no_symbol.find("native library 'marmot_test_native_lacking' does not export 'marmot_test_nothing'") != std::string::npos);
}

TEST_CASE("Only the libraries a program imports decide whether it may spawn workers", "[ffi][native][worker]")
{
	const MidoriTest::TempProject project({
		MidoriTest::TempProjectFile("Unsafe.mmt", WorkerProgram("marmot_test_native_unsafe")),
		MidoriTest::TempProjectFile("Safe.mmt", WorkerProgram("marmot_test_native_safe")),
	});
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), project.Root() / PlatformFileName("marmot_test_native_unsafe"));
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), project.Root() / PlatformFileName("marmot_test_native_safe"));

	MidoriExecutable unsafe = Compile(project, "Unsafe.mmt");
	REQUIRE(Loads(unsafe, MidoriProgramLoader::NativeLibraryLocations{}));
	const std::string refused = Run(std::move(unsafe));
	CHECK(refused.find("native libraries are not declared thread_safe: 'marmot_test_native_unsafe'") != std::string::npos);

	// The unsafe library is still loaded in this process; it must not stop a
	// program that does not import it.
	// thread_safe comes from the build plan and is recorded in the program.
	MidoriExecutable safe = Compile(
		project,
		"Safe.mmt",
		CompilationInputs().WithNativeLibraryPolicies({ { "marmot_test_native_safe", NativeLibraryPolicy{ .m_thread_safe = true } } }));
	REQUIRE(safe.GetNativeLibraries()[0u].m_policy.m_thread_safe);
	REQUIRE(Loads(safe, MidoriProgramLoader::NativeLibraryLocations{}));
	CHECK(Run(std::move(safe)) == "ok 42\n");
}

TEST_CASE("A native library's file can be named directly, whatever it is called", "[ffi][native]")
{
	const MidoriTest::TempProject project({ MidoriTest::TempProjectFile("Main.mmt", AddingProgram("marmot_test_native_named")) });
	// Not the platform's file name for the library, and nowhere it is searched for.
	std::filesystem::create_directories(project.Path("bin"));
	std::filesystem::copy_file(TestLibraryFile("marmot_test_native"), project.Path("bin") / "renamed.bin");

	MidoriExecutable executable = Compile(project, "Main.mmt");
	REQUIRE(Loads(executable, MidoriProgramLoader::NativeLibraryLocations{ .m_files = { { "marmot_test_native_named", project.Path("bin") / "renamed.bin" } } }));
	CHECK(Run(std::move(executable)) == "ok");
}
