#include <catch2/catch_test_macros.hpp>

#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/Lexer/Lexer.h"
#include "Compiler/ModuleManager/ModuleManager.h"
#include "Compiler/Token/Token.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/SourceFixture.h"
#include "support/TempProject.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::expected<BuildGraph, CompilerError> GenerateBuildGraphFromFile(const std::filesystem::path& file_path, std::optional<size_t> jobs = std::nullopt)
	{
		std::ifstream input_file(file_path);
		if (!input_file.is_open())
		{
			return std::unexpected(CompilerError::Simple(CompilerStage::Module, "Could not open a test module."));
		}

		std::ostringstream buffer;
		buffer << input_file.rdbuf();

		MidoriTest::SourceFixture source_fixture(buffer.str(), file_path.string());
		MidoriResult::LexerResult lex_result = Lexer(std::string(source_fixture.SourceCode()), source_fixture.FileName()).Lex();
		if (!lex_result.has_value())
		{
			return std::unexpected(std::move(lex_result.error()));
		}

		return ModuleManager(std::move(lex_result.value()), source_fixture.FileName(), source_fixture.SourceLines(), MidoriDriver::EnvironmentCompilationInputs().WithJobs(jobs)).GenerateBuildGraph();
	}

	void CheckDiagnosticLocation(const CompilerError& error, const std::filesystem::path& expected_file_path, int expected_line)
	{
		REQUIRE(error.m_location.has_value());
		CHECK(error.m_location->m_file_name == expected_file_path.string());
		CHECK(error.m_location->m_line == expected_line);
	}

	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("Discovery reports errors in import traversal order across job counts", "[module][discovery]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"First.mmt\", \"Second.mmt\", \"Missing.mmt\" }\n"),
		MidoriTest::TempProjectFile("First.mmt", "def value = 1;\n"),
		MidoriTest::TempProjectFile("Second.mmt", "module Second\ndef value = \"unterminated\n")
	});
	const std::filesystem::path main = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	std::string expected;
	for (size_t jobs : { 1u, 2u, 4u })
	{
		MidoriResult::ModuleManagerResult graph = GenerateBuildGraphFromFile(main, jobs);
		REQUIRE(!graph.has_value());
		REQUIRE(graph.error().m_code == CompilerErrorCode::ModuleDeclarationMissing);
		CheckDiagnosticLocation(graph.error(), std::filesystem::weakly_canonical(project.Path("First.mmt")), 1);
		const std::string rendered(graph.error().Rendered());
		if (expected.empty())
		{
			expected = rendered;
		}
		REQUIRE(rendered == expected);
	}
}

TEST_CASE("Discovery attributes a shared prefetched open failure to the first importer", "[module][discovery]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"Nested.mmt\", \"Directory.mmt\" }\n"),
		MidoriTest::TempProjectFile("Nested.mmt", "module Nested\nimport { \"Directory.mmt\" }\n")
	});
	std::filesystem::create_directory(project.Path("Directory.mmt"));
	const std::filesystem::path main = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	std::string expected;
	for (size_t jobs : { 1u, 2u, 4u })
	{
		MidoriResult::ModuleManagerResult graph = GenerateBuildGraphFromFile(main, jobs);
		REQUIRE(!graph.has_value());
		REQUIRE(graph.error().m_code == CompilerErrorCode::ModuleImportFileOpenFailed);
		CheckDiagnosticLocation(graph.error(), std::filesystem::weakly_canonical(project.Path("Nested.mmt")), 2);
		const std::string rendered(graph.error().Rendered());
		if (expected.empty())
		{
			expected = rendered;
		}
		REQUIRE(rendered == expected);
	}
}

TEST_CASE("BuildGraph computes deterministic compilation tiers for shared dependencies", "[module][graph]")
{
	BuildGraph graph;

	BuildGraph::BuildNode core_node;
	core_node.m_file_name = "Core";
	graph.m_nodes.emplace(core_node.m_file_name, core_node);

	BuildGraph::BuildNode lib_node;
	lib_node.m_file_name = "Lib";
	lib_node.m_dependencies = { "Core" };
	graph.m_nodes.emplace(lib_node.m_file_name, lib_node);

	BuildGraph::BuildNode util_node;
	util_node.m_file_name = "Util";
	util_node.m_dependencies = { "Core" };
	graph.m_nodes.emplace(util_node.m_file_name, util_node);

	BuildGraph::BuildNode main_node;
	main_node.m_file_name = "Main";
	main_node.m_dependencies = { "Lib", "Util" };
	graph.m_nodes.emplace(main_node.m_file_name, main_node);

	const std::vector<std::vector<std::string>> expected_tiers
	{
		{ "Core" },
		{ "Lib", "Util" },
		{ "Main" }
	};

	REQUIRE(graph.GetCompilationTiers() == expected_tiers);
}

TEST_CASE("ModuleManager preserves dependency metadata and strips module statements from nodes", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./Lib.mmt\", \"./Util.mmt\" }\n"
			"use Lib.{PrintLine, Parse}\n"
			"def main = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Lib.mmt",
			"module Lib\n"
			"public export { PrintLine, Parse }\n"
			"def PrintLine = fn() -> Int => 0;\n"
			"def Parse = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Util.mmt",
			"module Util\n"
			"def value = 1;\n"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::filesystem::path lib_file_path = std::filesystem::weakly_canonical(project.Path("Lib.mmt"));
	const std::filesystem::path util_file_path = std::filesystem::weakly_canonical(project.Path("Util.mmt"));

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(main_file_path);
	if (!graph_result.has_value())
	{
		FAIL(std::string(graph_result.error().Rendered()));
	}

	const BuildGraph& graph = graph_result.value();
	REQUIRE(graph.m_nodes.size() == 3);
	REQUIRE(graph.m_nodes.contains(main_file_path.string()));
	REQUIRE(graph.m_nodes.contains(lib_file_path.string()));
	REQUIRE(graph.m_nodes.contains(util_file_path.string()));

	const BuildGraph::BuildNode& main_node = graph.m_nodes.at(main_file_path.string());
	REQUIRE(main_node.m_dependencies == std::vector<std::string>{ lib_file_path.string(), util_file_path.string() });

	REQUIRE(graph.m_use_imports.contains(main_file_path.string()));
	REQUIRE(graph.m_use_imports.at(main_file_path.string()).size() == 2);
	CHECK(graph.m_use_imports.at(main_file_path.string())[0].m_module_name == "Lib");
	CHECK(graph.m_use_imports.at(main_file_path.string())[0].m_symbol_name == "PrintLine");
	CHECK(graph.m_use_imports.at(main_file_path.string())[1].m_module_name == "Lib");
	CHECK(graph.m_use_imports.at(main_file_path.string())[1].m_symbol_name == "Parse");

	REQUIRE(graph.m_module_declarations.at(main_file_path.string()).ModuleName() == "Main");
	REQUIRE(graph.m_module_declarations.at(lib_file_path.string()).ModuleName() == "Lib");
	REQUIRE(graph.m_module_declarations.at(lib_file_path.string()).HasExport("PrintLine"));
	REQUIRE(graph.m_module_declarations.at(lib_file_path.string()).HasExport("Parse"));

	const std::vector<Token::Name> remaining_tokens = MidoriTest::CollectTokenNames(main_node.m_tokens);
	CHECK(std::ranges::find(remaining_tokens, Token::Name::MODULE) == remaining_tokens.end());
	CHECK(std::ranges::find(remaining_tokens, Token::Name::IMPORT) == remaining_tokens.end());
	CHECK(std::ranges::find(remaining_tokens, Token::Name::USE) == remaining_tokens.end());
	CHECK(std::ranges::find(remaining_tokens, Token::Name::DEF) != remaining_tokens.end());

	const std::vector<std::vector<std::string>> expected_tiers
	{
		{ lib_file_path.string(), util_file_path.string() },
		{ main_file_path.string() }
	};
	REQUIRE(graph.GetCompilationTiers() == expected_tiers);
}

TEST_CASE("ModuleManager preserves dotted module names in use imports", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./MathVector.mmt\" }\n"
			"use Math.Vector.{add, sub}\n"
			"def main = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"MathVector.mmt",
			"module Math.Vector\n"
			"public export { add, sub }\n"
			"def add = 1;\n"
			"def sub = 0;\n"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(main_file_path);
	if (!graph_result.has_value())
	{
		FAIL(std::string(graph_result.error().Rendered()));
	}

	const BuildGraph& graph = graph_result.value();
	REQUIRE(graph.m_use_imports.contains(main_file_path.string()));
	REQUIRE(graph.m_use_imports.at(main_file_path.string()).size() == 2u);
	CHECK(graph.m_use_imports.at(main_file_path.string())[0].m_module_name == "Math.Vector");
	CHECK(graph.m_use_imports.at(main_file_path.string())[0].m_symbol_name == "add");
	CHECK(graph.m_use_imports.at(main_file_path.string())[1].m_module_name == "Math.Vector");
	CHECK(graph.m_use_imports.at(main_file_path.string())[1].m_symbol_name == "sub");
}

TEST_CASE("ModuleManager requires an explicit module declaration in every file", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"MissingModule.mmt",
			"def value = 1;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("MissingModule.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleDeclarationMissing;
	expectation.m_message_substrings = { "Module declaration required", "exactly one" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager rejects multiple module declarations in one file", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"MultipleModules.mmt",
			"module First\n"
			"module Second\n"
			"def value = 1;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("MultipleModules.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleDeclarationDuplicate;
	expectation.m_message_substrings = { "Multiple module declarations", "exactly one" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager requires the module declaration to be the first top-level statement", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"LateModule.mmt",
			"def value = 1;\n"
			"module Late\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("LateModule.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_message_substrings = { "first top-level statement" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager returns a diagnostic for reserved-keyword module names", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile(
			"Invalid.mmt",
			"module def\n"
			"def value = 1;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("Invalid.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_message_substrings = { "reserved keyword", "cannot be used as a module name" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager returns a diagnostic for malformed dotted use syntax", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"InvalidUse.mmt",
			"module Main\n"
			"use Math..{add}\n"
			"def value = 1;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("InvalidUse.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_rendered_substrings = { "use statement" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager returns a diagnostic for import statements without braces", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"InvalidImport.mmt",
			"module Main\n"
			"import <IO>\n"
			"def value = 1;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("InvalidImport.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	CheckDiagnosticLocation(error, std::filesystem::weakly_canonical(project.Path("InvalidImport.mmt")), 2);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Expected '{' after 'import'." };
	expectation.m_rendered_substrings = { "import <IO>", "import { <IO> }" };
	RequireErrorMatches(error, expectation);
}

TEST_CASE("ModuleManager preserves imported child lexer diagnostics across recursive imports", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./Parent.mmt\" }\n"
			"def main = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Parent.mmt",
			"module Parent\n"
			"import { \"./Broken.mmt\" }\n"
			"def value = 1;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Broken.mmt",
			"module Broken\n"
			"def value = @;\n"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::filesystem::path parent_file_path = std::filesystem::weakly_canonical(project.Path("Parent.mmt"));
	const std::filesystem::path broken_file_path = std::filesystem::weakly_canonical(project.Path("Broken.mmt"));

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(main_file_path);
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	CHECK(error.m_stage == CompilerStage::Lexer);
	CheckDiagnosticLocation(error, broken_file_path, 2);
	CHECK(error.m_message.find("Invalid character: @") != std::string::npos);
	CHECK(error.m_location->m_file_name != main_file_path.string());
	CHECK(error.m_location->m_file_name != parent_file_path.string());
}

TEST_CASE("ModuleManager preserves imported child module diagnostics across recursive imports", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./Parent.mmt\" }\n"
			"def main = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Parent.mmt",
			"module Parent\n"
			"import { \"./Broken.mmt\" }\n"
			"def value = 1;\n"
		),
		MidoriTest::TempProjectFile
		(
			"Broken.mmt",
			"def value = 1;\n"
		)
	});

	const std::filesystem::path main_file_path = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	const std::filesystem::path parent_file_path = std::filesystem::weakly_canonical(project.Path("Parent.mmt"));
	const std::filesystem::path broken_file_path = std::filesystem::weakly_canonical(project.Path("Broken.mmt"));

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(main_file_path);
	REQUIRE_FALSE(graph_result.has_value());

	const CompilerError& error = graph_result.error();
	CHECK(error.m_stage == CompilerStage::Module);
	CheckDiagnosticLocation(error, broken_file_path, 1);
	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleDeclarationMissing;
	expectation.m_message_substrings = { "Module declaration required" };
	RequireErrorMatches(error, expectation);
	CHECK(error.m_location->m_file_name != main_file_path.string());
	CHECK(error.m_location->m_file_name != parent_file_path.string());
}

TEST_CASE("ModuleManager tags unresolved imports with a stable diagnostic code", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./Missing.mmt\" }\n"
			"def main = fn() -> Int => 0;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("Main.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleImportResolutionFailed;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Could not resolve import" };
	RequireErrorMatches(graph_result.error(), expectation);
}

TEST_CASE("ModuleManager tags import file open failures with a stable diagnostic code", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./Library\" }\n"
			"def main = fn() -> Int => 0;\n"
		)
	});

	std::filesystem::create_directories(project.Path("Library"));

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("Main.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleImportFileOpenFailed;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Could not open import file" };
	RequireErrorMatches(graph_result.error(), expectation);
}

TEST_CASE("ModuleManager tags circular dependencies with a stable diagnostic code", "[module][graph]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile
		(
			"Main.mmt",
			"module Main\n"
			"import { \"./A.mmt\" }\n"
			"def main = fn() -> Int => 0;\n"
		),
		MidoriTest::TempProjectFile
		(
			"A.mmt",
			"module A\n"
			"import { \"./B.mmt\" }\n"
			"def value = 1;\n"
		),
		MidoriTest::TempProjectFile
		(
			"B.mmt",
			"module B\n"
			"import { \"./A.mmt\" }\n"
			"def value = 2;\n"
		)
	});

	std::expected<BuildGraph, CompilerError> graph_result = GenerateBuildGraphFromFile(project.Path("Main.mmt"));
	REQUIRE_FALSE(graph_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleCircularDependency;
	expectation.m_message_substrings = { "Circular dependency detected" };
	RequireErrorMatches(graph_result.error(), expectation);
}

TEST_CASE("Discovery reports cycles at their owning module across job counts", "[module][discovery]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"A.mmt\" }\n"),
		MidoriTest::TempProjectFile("A.mmt", "module A\nimport { \"B.mmt\" }\n"),
		MidoriTest::TempProjectFile("B.mmt", "module B\nimport { \"C.mmt\" }\n")
	});
	std::filesystem::path owner = project.Path("B.mmt");

	SECTION("A nested cycle is attributed to its nearest ancestor")
	{
		static_cast<void>(project.WriteSourceFile("C.mmt", "module C\nimport { \"B.mmt\" }\n"));
	}
	SECTION("A cycle through the entry is attributed to the entry")
	{
		static_cast<void>(project.WriteSourceFile("C.mmt", "module C\nimport { \"Main.mmt\" }\n"));
		owner = project.Path("Main.mmt");
	}
	SECTION("A self import is attributed to the importing module")
	{
		static_cast<void>(project.WriteSourceFile("C.mmt", "module C\nimport { \"C.mmt\" }\n"));
		owner = project.Path("C.mmt");
	}

	std::string expected;
	for (size_t jobs : { 1u, 2u, 4u })
	{
		MidoriResult::ModuleManagerResult graph = GenerateBuildGraphFromFile(project.Path("Main.mmt"), jobs);
		REQUIRE(!graph.has_value());
		REQUIRE(graph.error().m_code == CompilerErrorCode::ModuleCircularDependency);
		CheckDiagnosticLocation(graph.error(), std::filesystem::weakly_canonical(owner), 0);
		const std::string rendered(graph.error().Rendered());
		if (expected.empty())
		{
			expected = rendered;
		}
		REQUIRE(rendered == expected);
	}
}

TEST_CASE("Discovery preserves cycle error precedence within and after its owning module", "[module][discovery]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"A.mmt\" }\n"),
		MidoriTest::TempProjectFile("A.mmt", "module A\nimport { \"B.mmt\" }\n"),
		MidoriTest::TempProjectFile("B.mmt", "module B\nimport { \"A.mmt\" }\n")
	});
	CompilerErrorCode code = CompilerErrorCode::ModuleCircularDependency;
	int line = 0;
	SECTION("An error inside the cycle's owner takes precedence")
	{
		static_cast<void>(project.WriteSourceFile("A.mmt", "module A\nimport { \"B.mmt\", \"Missing.mmt\" }\n"));
		code = CompilerErrorCode::ModuleImportResolutionFailed;
		line = 2;
	}
	SECTION("An error after the cycle's owner does not take precedence")
	{
		static_cast<void>(project.WriteSourceFile("Main.mmt", "module Main\nimport { \"A.mmt\", \"Missing.mmt\" }\n"));
	}

	for (size_t jobs : { 1u, 2u, 4u })
	{
		MidoriResult::ModuleManagerResult graph = GenerateBuildGraphFromFile(project.Path("Main.mmt"), jobs);
		REQUIRE(!graph.has_value());
		REQUIRE(graph.error().m_code == code);
		CheckDiagnosticLocation(graph.error(), std::filesystem::weakly_canonical(project.Path("A.mmt")), line);
	}
}

TEST_CASE("Discovery retains shared and duplicate import edges once", "[module][discovery]")
{
	const MidoriTest::TempProject project
	({
		MidoriTest::TempProjectFile("Main.mmt", "module Main\nimport { \"Left.mmt\", \"Right.mmt\", \"Left.mmt\" }\n"),
		MidoriTest::TempProjectFile("Left.mmt", "module Left\nimport { \"Shared.mmt\" }\n"),
		MidoriTest::TempProjectFile("Right.mmt", "module Right\nimport { \"Shared.mmt\", \"Left.mmt\" }\n"),
		MidoriTest::TempProjectFile("Shared.mmt", "module Shared\npublic export { Value }\ndef Value = 1;\n")
	});
	const std::string main = std::filesystem::weakly_canonical(project.Path("Main.mmt")).string();
	const std::string left = std::filesystem::weakly_canonical(project.Path("Left.mmt")).string();
	const std::string right = std::filesystem::weakly_canonical(project.Path("Right.mmt")).string();
	const std::string shared = std::filesystem::weakly_canonical(project.Path("Shared.mmt")).string();
	for (size_t jobs : { 1u, 2u, 4u })
	{
		MidoriResult::ModuleManagerResult graph = GenerateBuildGraphFromFile(main, jobs);
		REQUIRE(graph.has_value());
		REQUIRE(graph->m_nodes.size() == 4u);
		REQUIRE(graph->m_module_declarations.size() == 4u);
		REQUIRE(graph->m_nodes.at(main).m_dependencies == std::vector<std::string>{ left, right });
		REQUIRE(graph->m_nodes.at(right).m_dependencies == std::vector<std::string>{ shared, left });
		REQUIRE(graph->m_nodes.at(shared).m_in_degree == 2);
		REQUIRE(graph->m_nodes.at(left).m_in_degree == 2);
		REQUIRE(graph->m_module_declarations.at(shared).HasExport("Value"));
		REQUIRE(graph->GetCompilationTiers() == std::vector<std::vector<std::string>>{ { shared }, { left }, { right }, { main } });
	}
}

TEST_CASE("Compiler tags missing exported symbols with a stable module diagnostic code", "[module][graph]")
{
	const std::string source_code =
		R"(module MissingExport
public export { missing }
def main = fn() -> Int => 0;
)";

	MidoriResult::CompilerResult compile_result = MidoriTest::CompileSnippet(source_code, "MissingExport.mmt");
	REQUIRE_FALSE(compile_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Module;
	expectation.m_code = CompilerErrorCode::ModuleMissingExportedSymbol;
	expectation.m_message_substrings = { "exported but not defined", "missing" };
	RequireErrorMatches(compile_result.error().First(), expectation);
}
