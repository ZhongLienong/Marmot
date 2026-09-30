#include <catch2/catch_test_macros.hpp>

#include "Compiler/AbstractSyntaxTree/Printer/AbstractSyntaxTreePrinter.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"

#include <filesystem>
#include <string>
#include <vector>

// The printer was compiled only in builds that asked for an AST dump, so it fell
// behind the tree for a long time without anyone noticing. These keep every
// kind of node in front of it in every build.

TEST_CASE("The AST printer writes classes, instances, aliases and comprehensions", "[ast][printer]")
{
	std::expected<MidoriTest::TypedSnippet, CompilerError> typed = MidoriTest::TypeCheckSnippet(R"(module Shapes
alias Meters = Int;

class Describe<T>
{
	Describe: fn(value: T) -> Text;
};

instance Describe<Int>
{
	def Describe = fn(value: Int) -> Text => value as Text;
};

def squares = [x * x for x in 0..1..4];
)", "Shapes.mmt");
	if (!typed.has_value())
	{
		FAIL(std::string(typed.error().Rendered()));
	}

	const std::string printed = AbstractSyntaxTreePrinter("Shapes", typed->m_program).Print();
	CHECK(printed.starts_with("module Shapes\n"));
	CHECK(printed.contains("TypeAlias {\n Name: Meters\n GenericParameters: \n AliasedType: Int\n}\n"));
	CHECK(printed.contains("Class {\n Name: Describe\n TypeParameters: T\n"));
	CHECK(printed.contains("   ReturnType: Text\n  }\n}\n"));
	CHECK(printed.contains("Instance {\n Class: Describe\n TypeArguments: Int\n"));
	CHECK(printed.contains("  ArrayComprehension {\n   Variable: x\n"));
}

TEST_CASE("Every prelude module prints its syntax tree", "[ast][printer][prelude]")
{
	const std::filesystem::path prelude(MARMOT_PRELUDE_DIR);
	std::vector<std::filesystem::path> modules;
	for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(prelude))
	{
		if (entry.path().extension() == ".mmt")
		{
			modules.push_back(entry.path());
		}
	}
	REQUIRE(modules.size() > 10u);

	for (const std::filesystem::path& module : modules)
	{
		INFO(module.string());
		MidoriDriver::CompileFileWithReportResult compiled = MidoriDriver::CompileFileWithReport(module, MidoriDriver::EnvironmentCompilationInputs().WithEmitAst(true));
		if (!compiled.has_value())
		{
			FAIL(compiled.error().Rendered());
		}
		REQUIRE_FALSE(compiled->m_ast.empty());
		CHECK(compiled->m_ast.back().starts_with("module "));
	}
}
