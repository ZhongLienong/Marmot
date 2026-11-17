#include <catch2/catch_test_macros.hpp>

#include "support/CompileHelpers.h"

TEST_CASE("Expected types stay local to statements, conditions and tuple elements", "[typechecker][checking-context]")
{
	const std::string source = R"(module ExpectedContexts
def choose: fn(Bool) -> (Array<Int>, Array<Text>) = fn(flag) => {
    def number: Int = 1;
    if (fn(value: Int) -> Bool => value > 0)(number) then
        if flag then ([number], ["first"]) else ([2], ["second"])
    else ([], [])
};
def identities: (fn(Int) -> Int, fn(Text) -> Text) =
    (fn(value) => value, fn(value) => value);
type Slot<T> = Empty | Full(T);
def slots: Array<Slot<Int>> = [Slot::Empty() for index in 0..1..3];
def scaled = fn(factor: Int) -> Array<fn() -> Int> =>
    [fn() -> Int => index * factor for index in 0..1..3];
)";
	const std::expected<MarmotTest::TypedSnippet, CompilerError> checked = MarmotTest::TypeCheckSnippet(source);
	if (!checked.has_value())
	{
		FAIL(checked.error().Rendered());
	}
}

TEST_CASE("An expected-type failure leaves later declaration contexts intact", "[typechecker][checking-context]")
{
	const std::string source = R"(module FailedExpectedContext
def broken: (fn(Int) -> Int, fn(Text) -> Text) =
    (fn(value) => value, fn(value) => 1);
def later: fn(Int) -> Bool = fn(value) => value;
)";
	const std::expected<MarmotTest::TypedSnippet, MarmotResult::CompilerDiagnostics> checked = MarmotTest::TypeCheckSnippetWithDiagnostics(source);
	REQUIRE_FALSE(checked.has_value());
	INFO(checked.error().Rendered());
	REQUIRE(checked.error().Size() == 2u);
	CHECK(checked.error().m_items[0u].m_code == CompilerErrorCode::TypeMismatch);
	CHECK(checked.error().m_items[0u].m_message.find("Text") != std::string::npos);
	CHECK(checked.error().m_items[1u].m_code == CompilerErrorCode::TypeMismatch);
	CHECK(checked.error().m_items[1u].m_message.find("Bool") != std::string::npos);
}

TEST_CASE("Deferred generic values retain use-site assumptions without lending them to later bodies", "[typechecker][checking-context]")
{
	const std::string declarations = R"(module DeferredContext
class Show<T> { show: fn(value: T) -> Text; };
def Keep = fn<T>(show: fn(T) -> Text) -> fn(T) -> Text => show;
def Render = fn<T>(value: T) -> Text where Show<T> => Show::show(value);
def WithShow = fn<T>(sample: T) -> fn(T) -> Text where Show<T> => Keep(Render);
)";
	const std::expected<MarmotTest::TypedSnippet, CompilerError> checked = MarmotTest::TypeCheckSnippet(declarations);
	if (!checked.has_value())
	{
		FAIL(checked.error().Rendered());
	}

	const std::string source = declarations + R"(def WithoutShow = fn(sample: Int) -> fn(Int) -> Text => Keep(Render);
)";
	const std::expected<MarmotTest::TypedSnippet, MarmotResult::CompilerDiagnostics> failed = MarmotTest::TypeCheckSnippetWithDiagnostics(source);
	REQUIRE_FALSE(failed.has_value());
	INFO(failed.error().Rendered());
	REQUIRE(failed.error().Size() == 1u);
	CHECK(failed.error().First().m_code == CompilerErrorCode::TypeUnsatisfiedConstraint);
	CHECK(failed.error().First().m_message.find("Show<Int>") != std::string::npos);
}
