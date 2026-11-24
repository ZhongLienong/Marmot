#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"

#include <format>
#include <string_view>
#include <tuple>

namespace
{
	void CheckAnchor(const Token& token, const Token& anchor)
	{
		CHECK(token.m_file_name == anchor.m_file_name);
		CHECK(token.m_line == anchor.m_line);
		CHECK(token.m_column == anchor.m_column);
		CHECK(token.m_source_length == anchor.m_source_length);
	}
}

TEST_CASE("Deriving preserves target order and type-name source anchors", "[parser][deriving]")
{
	const std::string source = R"(module DerivedOrder
type Box<T> = Empty | Full(T) deriving (Transferable, Map, Equatable, Bind, Unwrap);
def after = 0;
)";
	std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet(source, "DerivedOrder.mmt");
	REQUIRE(parsed.has_value());
	const MarmotProgramTree& program = parsed->m_program;
	REQUIRE(program.size() == 7u);
	REQUIRE(program[0]->IsStatement<MarmotStatement::Union>());
	const Token& anchor = program[0]->GetStatement<MarmotStatement::Union>().m_name;
	REQUIRE(program[1]->IsStatement<MarmotStatement::Instance>());
	const MarmotStatement::Instance& marker = program[1]->GetStatement<MarmotStatement::Instance>();
	CHECK(marker.m_class_name.m_lexeme == "Transferable");
	CHECK(marker.m_methods.empty());
	CheckAnchor(marker.m_class_name, anchor);
	REQUIRE(program[3]->IsStatement<MarmotStatement::Instance>());
	const MarmotStatement::Instance& equality = program[3]->GetStatement<MarmotStatement::Instance>();
	CHECK(equality.m_class_name.m_lexeme == "Equatable");
	CheckAnchor(equality.m_class_name, anchor);
	REQUIRE(equality.m_methods.size() == 1u);
	CHECK(equality.m_methods[0].m_name.m_lexeme == "Equals");
	CheckAnchor(equality.m_methods[0].m_name, anchor);
	CheckAnchor(equality.m_methods[0].m_definition.m_name, anchor);
	const std::vector<size_t> indices{ 2u, 4u, 5u };
	const std::vector<std::string> names{ "BoxMap", "BoxBind", "BoxUnwrap" };
	for (size_t index = 0u; index < indices.size(); index += 1u)
	{
		REQUIRE(program[indices[index]]->IsStatement<MarmotStatement::FunctionDefinition>());
		const MarmotStatement::FunctionDefinition& function = program[indices[index]]->GetStatement<MarmotStatement::FunctionDefinition>();
		CHECK(function.m_name.m_lexeme == names[index]);
		CheckAnchor(function.m_name, anchor);
		REQUIRE_FALSE(function.m_params.Empty());
		CheckAnchor(function.m_params.SourceToken(0), anchor);
		REQUIRE(function.m_body->IsExpression<MarmotExpression::Match>());
		CheckAnchor(function.m_body->GetExpression<MarmotExpression::Match>().m_match_keyword, anchor);
	}
	REQUIRE(program[6]->IsStatement<MarmotStatement::VariableDefinition>());
	CHECK(program[6]->GetStatement<MarmotStatement::VariableDefinition>().m_name.m_lexeme == "after");
}

TEST_CASE("Deriving preserves each class target's generated method shape", "[parser][deriving]")
{
	const std::tuple<std::string_view, std::string_view, size_t, size_t> cases[]
	{
		{ "Transferable", "", 0u, 1u },
		{ "Equatable", "Bool", 2u, 1u },
		{ "Hashable", "Int", 1u, 1u },
		{ "Orderable", "DeriveTargets::Outcome", 2u, 1u },
		{ "Convertable", "Text", 1u, 2u }
	};
	for (const std::string_view declaration : { "type Sample = { value: Int }", "type Sample = Empty | Full(Int)" })
	{
		for (const auto& [target, return_type, parameter_count, type_argument_count] : cases)
		{
			INFO(declaration);
			INFO(target);
			const bool orders = target == "Orderable";
			std::string source = std::format("module DeriveTargets\n{} deriving ({});\n", declaration, target);
			if (orders)
			{
				source += R"(class Orderable<T> { Compare: fn(a: T, b: T) -> Outcome; };
type Outcome = Unordered | Greater | Equal | Less;
)";
			}
			const std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet(source);
			REQUIRE(parsed.has_value());
			REQUIRE(parsed->m_program.size() == (orders ? 4u : 2u));
			REQUIRE(parsed->m_program[1]->IsStatement<MarmotStatement::Instance>());
			const MarmotStatement::Instance& instance = parsed->m_program[1]->GetStatement<MarmotStatement::Instance>();
			CHECK(instance.m_class_name.m_lexeme == target);
			CHECK(instance.m_type_args.size() == type_argument_count);
			if (return_type.empty())
			{
				CHECK(instance.m_methods.empty());
				continue;
			}
			REQUIRE(instance.m_methods.size() == 1u);
			const MarmotStatement::FunctionDefinition& method = instance.m_methods[0].m_definition;
			CHECK(method.m_params.Size() == parameter_count);
			CHECK(method.m_return_type->ToString() == return_type);
		}
	}
}

TEST_CASE("Deriving rejects unsupported targets for each declaration shape", "[parser][deriving]")
{
	for (const std::string_view target : { "Map", "Bind", "Unwrap", "Bogus", "Numeric" })
	{
		INFO(target);
		const std::string source = std::format("module InvalidRecord\ntype Sample = {{ value: Int }} deriving ({});\n", target);
		const std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet(source);
		REQUIRE_FALSE(parsed.has_value());
		CHECK(parsed.error().m_message == "Unsupported deriving target for struct.");
	}
	for (const std::string_view target : { "Bogus", "Numeric", "map" })
	{
		INFO(target);
		const std::string source = std::format("module InvalidUnion\ntype Sample<T> = Empty | Full(T) deriving ({});\n", target);
		const std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet(source);
		REQUIRE_FALSE(parsed.has_value());
		CHECK(parsed.error().m_message == "Unsupported deriving target for union.");
	}
}

TEST_CASE("Deriving reserves container names before validating their bodies", "[parser][deriving]")
{
	for (const std::string& target : { std::string("Bind"), std::string("Unwrap") })
	{
		INFO(target);
		const std::string declaration = std::format("type PairBox<T> = Empty | Pair(T, T) deriving ({});\n", target);
		const std::string source = std::format("module Collision\ndef PairBox{} = 0;\n", target) + declaration;
		std::expected<MarmotTest::ParsedSnippet, CompilerError> collision = MarmotTest::ParseSnippet(source);
		REQUIRE_FALSE(collision.has_value());
		CHECK(collision.error().m_stage == CompilerStage::Parser);
		CHECK(collision.error().m_message == "Name already exists in the current scope");
		REQUIRE(collision.error().m_location.has_value());
		CHECK(collision.error().m_location->m_line == 3);
		CHECK(collision.error().m_location->m_column == 5);
		CHECK(collision.error().m_location->m_caret_length == 7u);
		std::expected<MarmotTest::ParsedSnippet, CompilerError> invalid = MarmotTest::ParseSnippet("module Shape\n" + declaration);
		REQUIRE_FALSE(invalid.has_value());
		CHECK(invalid.error().m_message == target + " deriving requires variants to contain at most one direct field of the first type parameter.");
	}
}

TEST_CASE("Deriving checks container type parameters before reserving names", "[parser][deriving]")
{
	std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet("module NoParameters\ndef BoxMap = 0;\ntype Box = Empty | Other deriving (Map);\n");
	REQUIRE_FALSE(parsed.has_value());
	CHECK(parsed.error().m_message == "Container deriving requires at least one union type parameter.");
}

TEST_CASE("Deriving rejects local declarations before unsupported targets", "[parser][deriving]")
{
	for (const std::string& declaration : { std::string("type Local<T> = Empty | Full(T) deriving (Bogus);"), std::string("type Local = { value: Int } deriving (Bogus);") })
	{
		const std::string source = "module LocalDeriving\ndef f = fn() -> Int => {\n    " + declaration + "\n    0\n};\n";
		std::expected<MarmotTest::ParsedSnippet, CompilerError> parsed = MarmotTest::ParseSnippet(source);
		REQUIRE_FALSE(parsed.has_value());
		CHECK(parsed.error().m_message == "Deriving declarations are only supported at global scope.");
		REQUIRE(parsed.error().m_location.has_value());
		CHECK(parsed.error().m_location->m_line == 3);
		CHECK(parsed.error().m_location->m_column == 9);
		CHECK(parsed.error().m_location->m_caret_length == 5u);
	}
}
