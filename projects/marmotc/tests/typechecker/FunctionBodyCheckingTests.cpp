#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"

TEST_CASE("Function body failures restore constraints before checking the next declaration", "[typechecker][function-body]")
{
	const std::string declarations = R"(module FunctionBodyFailure
class Show<T> { show: fn(value: T) -> Text; };
class Check<T> { check: fn(value: T) -> Int; };
def Display = fn<T>(value: T) -> Text where Show<T> => Show::show(value);
)";
	const std::vector<std::string> failing_bodies
	{
		R"(def Broken = fn<T>(value: T) -> Int where Show<Int> => "wrong";
)",
		R"(instance Check<Int> where Show<Int> {
    def check = fn(value: Int) -> Int => "wrong";
};
)",
		R"(def Broken = fn<T>(value: T) -> Int where Show<Int> => {
    def flag: Bool = 1;
    0
};
)",
		R"(instance Check<Int> where Show<Int> {
    def check = fn(value: Int) -> Int => {
        def flag: Bool = 1;
        0
    };
};
)"
	};
	for (const std::string& body : failing_bodies)
	{
		INFO(body);
		const std::string source = declarations + body + R"(def after = fn(value: Int) -> Text => Display(value);
)";
		const std::expected<MarmotTest::TypedSnippet, MarmotResult::CompilerDiagnostics> checked = MarmotTest::TypeCheckSnippetWithDiagnostics(source, "FunctionBodyFailure.mmt");
		REQUIRE_FALSE(checked.has_value());
		INFO(checked.error().Rendered());
		REQUIRE(checked.error().Size() == 2u);
		CHECK(checked.error().m_items[0u].m_code == CompilerErrorCode::TypeMismatch);
		CHECK(checked.error().m_items[1u].m_code == CompilerErrorCode::TypeUnsatisfiedConstraint);
		CHECK(checked.error().m_items[1u].m_message.find("Show<Int>") != std::string::npos);
	}
}

TEST_CASE("Successful function bodies do not discharge constraints of later declarations", "[typechecker][function-body]")
{
	const std::string source = R"(module SuccessfulFunctionBody
class Show<T> { show: fn(value: T) -> Text; };
def Display = fn<T>(value: T) -> Text where Show<T> => Show::show(value);
def First = fn<T>(value: T) -> Int where Show<Int> => 1;
def after = fn(value: Int) -> Text => Display(value);
)";
	const std::expected<MarmotTest::TypedSnippet, MarmotResult::CompilerDiagnostics> checked = MarmotTest::TypeCheckSnippetWithDiagnostics(source);
	REQUIRE_FALSE(checked.has_value());
	INFO(checked.error().Rendered());
	REQUIRE(checked.error().Size() == 1u);
	CHECK(checked.error().First().m_code == CompilerErrorCode::TypeUnsatisfiedConstraint);
}

TEST_CASE("Nested generic lambdas keep their own and enclosing constraints separate", "[typechecker][function-body]")
{
	const std::string source = R"(module NestedFunctionBody
class Echo<T> { echo: fn(value: T) -> T; };
def Outer = fn<T>(value: T) -> T where Echo<T> => {
    def inner = fn<U>(input: U) -> U where Echo<U> => Echo::echo(input);
    def other = fn(input: Int) -> Text => "text";
    inner(value)
};
)";
	const std::expected<MarmotTest::TypedSnippet, CompilerError> checked = MarmotTest::TypeCheckSnippet(source);
	if (!checked.has_value())
	{
		FAIL(checked.error().Rendered());
	}
}

TEST_CASE("A failed inner lambda does not change the diagnostics of later function bodies", "[typechecker][function-body]")
{
	const std::string source = R"(module FailedNestedFunctionBody
def Broken = fn<T>(value: T) -> T => {
    def inner = fn(input: Int) -> Text => input;
    value
};
def later = fn(value: Int) -> Bool => value;
)";
	const std::expected<MarmotTest::TypedSnippet, MarmotResult::CompilerDiagnostics> checked = MarmotTest::TypeCheckSnippetWithDiagnostics(source);
	REQUIRE_FALSE(checked.has_value());
	INFO(checked.error().Rendered());
	REQUIRE(checked.error().Size() == 2u);
	CHECK(checked.error().m_items[0u].m_message.find("Expected type 'Text' but got 'Int'") != std::string::npos);
	CHECK(checked.error().m_items[1u].m_message.find("Expected type 'Bool' but got 'Int'") != std::string::npos);
}

TEST_CASE("Generic and inferred lambdas keep their separate signature finalization", "[typechecker][function-body]")
{
	const std::string source = R"(module FunctionBodyFinalization
def Identity = fn<T>(value: T) -> T => value;
def number = Identity(1);
def text = Identity("text");
def inferred: fn(Int) -> Text = fn(value) => "text";
def result = inferred(number);
)";
	const std::expected<MarmotTest::TypedSnippet, CompilerError> checked = MarmotTest::TypeCheckSnippet(source);
	if (!checked.has_value())
	{
		FAIL(checked.error().Rendered());
	}
	const MarmotStatement::VariableDefinition& definition = checked->m_program.back()->GetStatement<MarmotStatement::VariableDefinition>();
	CHECK(definition.m_value->GetType()->IsType<MarmotType::TextType>());
}
