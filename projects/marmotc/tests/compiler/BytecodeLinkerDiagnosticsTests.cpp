#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "support/DiagnosticMatchers.h"

namespace
{
	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("Bytecode linker reports unresolved imports with importer source provenance", "[compiler][linker][diagnostics]")
{
	BytecodeModule importer("Importer", "Importer.mmt");
	importer.m_imports.emplace_back(
		"missingValue",
		"MissingModule",
		BytecodeModule::SourceProvenance(
			4,
			25,
			12u,
			std::string("def result = MissingModule::missingValue;")));

	std::vector<BytecodeModule> modules;
	modules.emplace_back(std::move(importer));

	MidoriResult::BytecodeLinkerResult link_result = BytecodeLinker(std::move(modules), "Importer").Link();
	REQUIRE_FALSE(link_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::BytecodeLinker;
	expectation.m_code = CompilerErrorCode::BytecodeLinkerUnresolvedImport;
	expectation.m_line = 4;
	expectation.m_message_substrings = { "Unresolved import: missingValue from module MissingModule." };
	expectation.m_rendered_substrings = { "Bytecode Linker Error", "Importer.mmt:4", "MissingModule::missingValue" };
	RequireErrorMatches(link_result.error(), expectation);
}

TEST_CASE("Bytecode linker tags empty link jobs with a stable code", "[compiler][linker][diagnostics]")
{
	MidoriResult::BytecodeLinkerResult link_result = BytecodeLinker({}, "Main").Link();
	REQUIRE_FALSE(link_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::BytecodeLinker;
	expectation.m_code = CompilerErrorCode::BytecodeLinkerNoModulesToLink;
	expectation.m_message_substrings = { "Cannot link: no modules were successfully compiled." };
	RequireErrorMatches(link_result.error(), expectation);
}

TEST_CASE("Bytecode linker reports duplicate exports with source-aware conflict details", "[compiler][linker][diagnostics]")
{
	BytecodeModule first_module("Shared", "SharedA.mmt");
	first_module.m_exports.emplace_back(
		"run",
		0uz,
		0uz,
		BytecodeModule::SymbolType::FUNCTION,
		BytecodeModule::SourceProvenance(
			2,
			6,
			3u,
			std::string("def run = fn() -> Int => 1;")));

	BytecodeModule second_module("Shared", "SharedB.mmt");
	second_module.m_exports.emplace_back(
		"run",
		0uz,
		0uz,
		BytecodeModule::SymbolType::FUNCTION,
		BytecodeModule::SourceProvenance(
			5,
			6,
			3u,
			std::string("def run = fn() -> Int => 2;")));

	std::vector<BytecodeModule> modules;
	modules.emplace_back(std::move(first_module));
	modules.emplace_back(std::move(second_module));

	MidoriResult::BytecodeLinkerResult link_result = BytecodeLinker(std::move(modules), "Shared").Link();
	REQUIRE_FALSE(link_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::BytecodeLinker;
	expectation.m_code = CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol;
	expectation.m_line = 5;
	expectation.m_message_substrings = { "Duplicate symbol export: run from module Shared.", "SharedA.mmt:2" };
	expectation.m_rendered_substrings = { "Bytecode Linker Error", "SharedB.mmt:5", "def run = fn() -> Int => 2;" };
	RequireErrorMatches(link_result.error(), expectation);
}
