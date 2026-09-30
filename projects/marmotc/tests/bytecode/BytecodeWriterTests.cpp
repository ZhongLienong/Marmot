#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "support/CompileHelpers.h"

#include <sstream>

TEST_CASE("The artifact writer emits deterministic bytes", "[bytecode-artifact]")
{
	const MidoriResult::CompilerResult compiled = MidoriTest::CompileSnippet("module Main\ndef greeting = \"hello\";\ndef Helper = fn() -> Int => 42;\n");
	REQUIRE(compiled.has_value());
	std::ostringstream first;
	std::ostringstream second;
	REQUIRE(MidoriBinaryArtifact::WriteExecutable(compiled.value(), first).has_value());
	REQUIRE(MidoriBinaryArtifact::WriteExecutable(compiled.value(), second).has_value());
	CHECK(first.str() == second.str());
	CHECK(first.str().substr(0u, 3u) == "MBC");
}

TEST_CASE("Source embedding changes the artifact without changing the executable", "[bytecode-artifact]")
{
	const MidoriResult::CompilerResult compiled = MidoriTest::CompileSnippet("module Main\ndef greeting = \"hello\";\n");
	REQUIRE(compiled.has_value());
	std::ostringstream with_sources;
	std::ostringstream without_sources;
	REQUIRE(MidoriBinaryArtifact::WriteExecutable(compiled.value(), with_sources, true).has_value());
	REQUIRE(MidoriBinaryArtifact::WriteExecutable(compiled.value(), without_sources, false).has_value());
	CHECK(with_sources.str().size() > without_sources.str().size());
	CHECK(with_sources.str().find("def greeting") != std::string::npos);
	CHECK(without_sources.str().find("def greeting") == std::string::npos);
}
