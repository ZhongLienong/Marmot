#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "support/CompileHelpers.h"

#include <sstream>

TEST_CASE("The artifact writer emits deterministic bytes", "[bytecode-artifact]")
{
	const MarmotResult::ExecutableDiagnosticsResult compiled = MarmotTest::CompileSnippet("module Main\ndef greeting = \"hello\";\ndef Helper = fn() -> Int => 42;\n");
	REQUIRE(compiled.HasValue());
	std::ostringstream first;
	std::ostringstream second;
	REQUIRE(MarmotBinaryArtifact::WriteExecutable(compiled.Value(), first).HasValue());
	REQUIRE(MarmotBinaryArtifact::WriteExecutable(compiled.Value(), second).HasValue());
	CHECK(first.str() == second.str());
	CHECK(first.str().substr(0u, 3u) == "MBC");
}

TEST_CASE("Source embedding changes the artifact without changing the executable", "[bytecode-artifact]")
{
	const MarmotResult::ExecutableDiagnosticsResult compiled = MarmotTest::CompileSnippet("module Main\ndef greeting = \"hello\";\n");
	REQUIRE(compiled.HasValue());
	std::ostringstream with_sources;
	std::ostringstream without_sources;
	REQUIRE(MarmotBinaryArtifact::WriteExecutable(compiled.Value(), with_sources, true).HasValue());
	REQUIRE(MarmotBinaryArtifact::WriteExecutable(compiled.Value(), without_sources, false).HasValue());
	CHECK(with_sources.str().size() > without_sources.str().size());
	CHECK(with_sources.str().find("def greeting") != std::string::npos);
	CHECK(without_sources.str().find("def greeting") == std::string::npos);
}
