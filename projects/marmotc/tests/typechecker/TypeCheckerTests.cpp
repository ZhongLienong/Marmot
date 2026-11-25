#include <catch2/catch_test_macros.hpp>

#include "Compiler/TypeChecker/TypeChecker.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"

#include <array>
#include <string>
#include <string_view>

namespace
{
	const MarmotStatement::VariableDefinition* FindVariableDefinition(const MarmotProgramTree& program, std::string_view name)
	{
		for (const std::unique_ptr<MarmotStatement>& statement : program)
		{
			if (statement == nullptr || !statement->IsStatement<MarmotStatement::VariableDefinition>())
			{
				continue;
			}

			const MarmotStatement::VariableDefinition& definition = statement->GetStatement<MarmotStatement::VariableDefinition>();
			if (definition.m_name.m_lexeme == name)
			{
				return std::addressof(definition);
			}
		}

		return nullptr;
	}

	void RequireErrorMatches(const CompilerError& error, const MarmotTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MarmotTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("TypeChecker rejects out-of-range Float literals in expressions and patterns", "[typechecker][literal]")
{
	const std::array<std::string, 2u> literals
	{
		"1" + std::string(309u, '0') + ".0",
		"0." + std::string(323u, '0') + "1",
	};
	for (const std::string& literal : literals)
	{
		const std::string expression = "module FloatRange\ndef value = " + literal + ";\n";
		const std::string pattern = "module FloatRange\ndef value = match 1.0 with case 0.0 => false case -" + literal + " => true case _ => false;\n";
		for (const std::string* source : { &expression, &pattern })
		{
			const MarmotResult::Result<MarmotTest::TypedSnippet> checked = MarmotTest::TypeCheckSnippet(*source);
			REQUIRE_FALSE(checked.HasValue());
			CHECK(checked.Error().m_stage == CompilerStage::TypeChecker);
			CHECK(checked.Error().m_message == "Float literal is out of range.");
		}
	}
}

TEST_CASE("Instance methods may contain their class name after an underscore", "[typechecker][instance-method-names]")
{
	const std::string source = R"(module MethodNames
class Echo<T> {
    Call_Echo: fn(value: T) -> T;
};
instance Echo<Int> {
    def Call_Echo = fn(value: Int) -> Int => value;
};
def result = Echo::Call_Echo(7);
)";
	const MarmotResult::Result<MarmotTest::TypedSnippet> checked = MarmotTest::TypeCheckSnippet(source);
	if (!checked.HasValue())
	{
		FAIL(checked.Error().Rendered());
	}
}

TEST_CASE("Parsed instance methods retain their source token and emitted symbol", "[parser][instance-method-names]")
{
	const std::string source = R"(module MethodTokens
class Echo<T> {
    Call_Echo: fn(value: T) -> T;
};
instance Echo<Int> {
    def Call_Echo = fn(value: Int) -> Int => value;
};
)";
	const MarmotResult::Result<MarmotTest::ParsedSnippet> parsed = MarmotTest::ParseSnippet(source, "MethodTokens.mmt");
	REQUIRE(parsed.HasValue());
	REQUIRE(parsed->m_program.size() == 2u);
	const MarmotStatement::Instance& instance = parsed->m_program[1]->GetStatement<MarmotStatement::Instance>();
	REQUIRE(instance.m_methods.size() == 1u);
	const MarmotStatement::Instance::Method& method = instance.m_methods[0];
	CHECK(method.m_name.m_lexeme == "Call_Echo");
	CHECK(method.m_definition.m_name.m_lexeme == "$Call_Echo_Echo_Int");
	CHECK(method.m_name.m_file_name == "MethodTokens.mmt");
	CHECK(method.m_name.m_line == 6);
	CHECK(method.m_name.m_column == 8);
	CHECK(method.m_name.m_source_length == 9u);
	CHECK(method.m_definition.m_name.m_column == method.m_name.m_column);
	CHECK(method.m_definition.m_name.m_source_length == method.m_name.m_source_length);
}

TEST_CASE("Instance method validation uses the complete source name", "[typechecker][instance-method-names]")
{
	SECTION("A name containing the class name must not match a shorter declared method")
	{
		const std::string source = R"(module UnknownMethod
class Echo<T> {
    Call: fn(value: T) -> T;
};
instance Echo<Int> {
    def Call_Echo = fn(value: Int) -> Int => value;
};
)";
		const MarmotResult::Result<MarmotTest::TypedSnippet> checked = MarmotTest::TypeCheckSnippet(source);
		REQUIRE_FALSE(checked.HasValue());
		CHECK(checked.Error().m_message == "Instance declaration error: method 'Call_Echo' not defined in typeclass");
	}
	SECTION("Signature diagnostics keep the complete method name")
	{
		const std::string source = R"(module MethodSignature
class Echo<T> {
    Call_Echo: fn(value: T) -> Text;
};
instance Echo<Int> {
    def Call_Echo = fn(value: Int) -> Int => value;
};
)";
		const MarmotResult::Result<MarmotTest::TypedSnippet> checked = MarmotTest::TypeCheckSnippet(source);
		REQUIRE_FALSE(checked.HasValue());
		CHECK(checked.Error().m_message == "Instance declaration error: method 'Call_Echo' return type does not match class declaration\nExpected Text, but got Int");
	}
}

TEST_CASE("TypeChecker infers lambda parameter and return types from an annotated binding", "[typechecker]")
{
	const std::string source_code =
		R"(module TypeInference
def doubler : fn(Int) -> Int = fn(x) => { x * 2 };
def result = doubler(21);
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "TypeInference.mmt");
	if (!typecheck_result.HasValue())
	{
		FAIL(std::string(typecheck_result.Error().Rendered()));
	}

	const MarmotStatement::VariableDefinition* doubler_definition = FindVariableDefinition(typecheck_result->m_program, "doubler");
	REQUIRE(doubler_definition != nullptr);
	REQUIRE(doubler_definition->m_value->IsExpression<MarmotExpression::Function>());

	const MarmotExpression::Function& lambda = doubler_definition->m_value->GetExpression<MarmotExpression::Function>();
	REQUIRE(lambda.m_param_types.size() == 1u);
	REQUIRE(lambda.m_param_types[0u]->IsType<MarmotType::IntegerType>());
	REQUIRE(lambda.m_return_type->IsType<MarmotType::IntegerType>());
	REQUIRE(doubler_definition->m_value->GetType()->IsType<MarmotType::FunctionType>());
	REQUIRE(doubler_definition->m_value->GetType()->ToString() == "transferable fn(Int) -> Int");

	const MarmotStatement::VariableDefinition* result_definition = FindVariableDefinition(typecheck_result->m_program, "result");
	REQUIRE(result_definition != nullptr);
	REQUIRE(result_definition->m_value->GetType()->IsType<MarmotType::IntegerType>());
}

TEST_CASE("TypeChecker resolves aliases of instantiated generic types to their underlying types", "[typechecker]")
{
	const std::string source_code =
		R"(module AliasTypes
type Pair<A, B> =
{
	first: A,
	second: B
};
alias IntPair = Pair<Int, Int>;
def pair : IntPair = Pair(1, 2);
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "AliasTypes.mmt");
	if (!typecheck_result.HasValue())
	{
		FAIL(std::string(typecheck_result.Error().Rendered()));
	}

	const MarmotStatement::VariableDefinition* pair_definition = FindVariableDefinition(typecheck_result->m_program, "pair");
	REQUIRE(pair_definition != nullptr);
	REQUIRE(pair_definition->m_annotated_type.HasValue());
	REQUIRE(pair_definition->m_annotated_type.Value()->IsType<MarmotType::StructType>());

	const MarmotType::StructType& pair_type = pair_definition->m_annotated_type.Value()->GetType<MarmotType::StructType>();
	REQUIRE(pair_type.m_name == "Pair");
	REQUIRE(pair_type.m_member_types.size() == 2u);
	REQUIRE(pair_type.m_member_types[0u]->IsType<MarmotType::IntegerType>());
	REQUIRE(pair_type.m_member_types[1u]->IsType<MarmotType::IntegerType>());
	REQUIRE(pair_definition->m_value->GetType()->IsType<MarmotType::StructType>());

	const MarmotType::StructType& constructed_pair_type = pair_definition->m_value->GetType()->GetType<MarmotType::StructType>();
	REQUIRE(constructed_pair_type.m_name == "Pair");
	REQUIRE(constructed_pair_type.m_member_types.size() == 2u);
	REQUIRE(constructed_pair_type.m_member_types[0u]->IsType<MarmotType::IntegerType>());
	REQUIRE(constructed_pair_type.m_member_types[1u]->IsType<MarmotType::IntegerType>());
}

TEST_CASE("TypeChecker reports deterministic class-constraint failures at the call site", "[typechecker]")
{
	const std::string source_code =
		R"(module ConstraintFailure
class Show<T>
{
	show: fn(value: T) -> Text;
};
type Hidden =
{
	value: Int
};
def Display = fn<T>(value: T) -> Text where Show<T> => {
	Show::show(value)
};
def rendered = Display(Hidden(1));
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "ConstraintFailure.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeUnsatisfiedConstraint;
	expectation.m_message_substrings = { "Type Hidden does not satisfy constraint Show<Hidden>", "no matching instance found" };
	expectation.m_rendered_substrings = { "Type Checker Error", "ConstraintFailure.mmt:13", "Display(Hidden(1))" };
	RequireErrorMatches(typecheck_result.Error(), expectation);
}

TEST_CASE("TypeChecker exposes failures through shared diagnostics transport", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module SharedDiagnostics
def number : Int = "oops";
)";

	MarmotResult::Result<MarmotTest::ParsedSnippet> parse_result = MarmotTest::ParseSnippet(source_code, "SharedDiagnostics.mmt");
	REQUIRE(parse_result.HasValue());

	MarmotResult::TypeCheckerResult typecheck_result = TypeChecker
	(
		std::move(parse_result->m_program),
		parse_result->m_source.FileName(),
		parse_result->m_source.SourceLines()
	).TypeCheck();

	REQUIRE_FALSE(typecheck_result.HasValue());
	REQUIRE(typecheck_result.Error().Size() == 1u);

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeMismatch;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Expected type 'Int' but got 'Text'" };
	expectation.m_rendered_substrings = { "Type Checker Error", "SharedDiagnostics.mmt:2" };
	RequireErrorMatches(typecheck_result.Error().First(), expectation);
}

TEST_CASE("TypeChecker preserves multiple top-level failures as separate diagnostics", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module MultipleDiagnostics
def number : Int = "oops";
def flag : Bool = 123;
)";

	MarmotResult::DiagnosticsResult<MarmotTest::TypedSnippet> typecheck_result =
		MarmotTest::TypeCheckSnippetWithDiagnostics(source_code, "MultipleDiagnostics.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());
	REQUIRE(typecheck_result.Error().Size() == 2u);

	MarmotTest::ErrorExpectation first_expectation;
	first_expectation.m_stage = CompilerStage::TypeChecker;
	first_expectation.m_code = CompilerErrorCode::TypeMismatch;
	first_expectation.m_line = 2;
	first_expectation.m_message_substrings = { "Expected type 'Int' but got 'Text'" };
	first_expectation.m_rendered_substrings = { "Type Checker Error", "MultipleDiagnostics.mmt:2" };
	RequireErrorMatches(typecheck_result.Error().m_items[0u], first_expectation);

	MarmotTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::TypeChecker;
	second_expectation.m_code = CompilerErrorCode::TypeMismatch;
	second_expectation.m_line = 3;
	second_expectation.m_message_substrings = { "Expected type 'Bool' but got 'Int'" };
	second_expectation.m_rendered_substrings = { "Type Checker Error", "MultipleDiagnostics.mmt:3" };
	RequireErrorMatches(typecheck_result.Error().m_items[1u], second_expectation);
}

TEST_CASE("Compiler tags undefined names with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module UndefinedName
def value = missing;
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "UndefinedName.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Parser;
	expectation.m_code = CompilerErrorCode::TypeUndefinedName;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Undefined name" };
	expectation.m_rendered_substrings = { "Parser Error", "UndefinedName.mmt:2" };
	RequireErrorMatches(typecheck_result.Error(), expectation);
}

TEST_CASE("TypeChecker tags non-callable values with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module NotCallable
def value = 1;
def result = value();
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "NotCallable.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeNotCallable;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "not a callable" };
	expectation.m_rendered_substrings = { "Type Checker Error", "NotCallable.mmt:3" };
	RequireErrorMatches(typecheck_result.Error(), expectation);
}

TEST_CASE("TypeChecker tags incorrect arity with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module IncorrectArity
def id = fn(value: Int) -> Int => value;
def result = id();
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "IncorrectArity.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeIncorrectArity;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "incorrect arity" };
	expectation.m_rendered_substrings = { "Type Checker Error", "IncorrectArity.mmt:3" };
	RequireErrorMatches(typecheck_result.Error(), expectation);
}

TEST_CASE("TypeChecker tags non-exhaustive matches with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module NonExhaustiveMatch
type Option = None | Some(Int);
def value = Option::Some(1);
def result = match value with
    case Option::Some(x) => x
;
)";

	MarmotResult::Result<MarmotTest::TypedSnippet> typecheck_result = MarmotTest::TypeCheckSnippet(source_code, "NonExhaustiveMatch.mmt");
	REQUIRE_FALSE(typecheck_result.HasValue());

	MarmotTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeNonExhaustiveMatch;
	expectation.m_line = 4;
	expectation.m_message_substrings = { "non-exhaustive match", "None" };
	expectation.m_rendered_substrings = { "Type Checker Error", "NonExhaustiveMatch.mmt:4" };
	RequireErrorMatches(typecheck_result.Error(), expectation);
}
