#include <catch2/catch_test_macros.hpp>

#include "Compiler/TypeChecker/TypeChecker.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"

#include <expected>
#include <string>
#include <string_view>

namespace
{
	const MidoriStatement::VariableDefinition* FindVariableDefinition(const MidoriProgramTree& program, std::string_view name)
	{
		for (const std::unique_ptr<MidoriStatement>& statement : program)
		{
			if (statement == nullptr || !statement->IsStatement<MidoriStatement::VariableDefinition>())
			{
				continue;
			}

			const MidoriStatement::VariableDefinition& definition = statement->GetStatement<MidoriStatement::VariableDefinition>();
			if (definition.m_name.m_lexeme == name)
			{
				return std::addressof(definition);
			}
		}

		return nullptr;
	}

	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("TypeChecker infers lambda parameter and return types from an annotated binding", "[typechecker]")
{
	const std::string source_code =
		R"(module TypeInference
def doubler : fn(Int) -> Int = fn(x) => { x * 2 };
def result = doubler(21);
)";

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "TypeInference.mmt");
	if (!typecheck_result.has_value())
	{
		FAIL(std::string(typecheck_result.error().Rendered()));
	}

	const MidoriStatement::VariableDefinition* doubler_definition = FindVariableDefinition(typecheck_result->m_program, "doubler");
	REQUIRE(doubler_definition != nullptr);
	REQUIRE(doubler_definition->m_value->IsExpression<MidoriExpression::Function>());

	const MidoriExpression::Function& lambda = doubler_definition->m_value->GetExpression<MidoriExpression::Function>();
	REQUIRE(lambda.m_param_types.size() == 1u);
	REQUIRE(lambda.m_param_types[0u]->IsType<MidoriType::IntegerType>());
	REQUIRE(lambda.m_return_type->IsType<MidoriType::IntegerType>());
	REQUIRE(doubler_definition->m_value->GetType()->IsType<MidoriType::FunctionType>());
	REQUIRE(doubler_definition->m_value->GetType()->ToString() == "fn(Int) -> Int");

	const MidoriStatement::VariableDefinition* result_definition = FindVariableDefinition(typecheck_result->m_program, "result");
	REQUIRE(result_definition != nullptr);
	REQUIRE(result_definition->m_value->GetType()->IsType<MidoriType::IntegerType>());
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

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "AliasTypes.mmt");
	if (!typecheck_result.has_value())
	{
		FAIL(std::string(typecheck_result.error().Rendered()));
	}

	const MidoriStatement::VariableDefinition* pair_definition = FindVariableDefinition(typecheck_result->m_program, "pair");
	REQUIRE(pair_definition != nullptr);
	REQUIRE(pair_definition->m_annotated_type.has_value());
	REQUIRE(pair_definition->m_annotated_type.value()->IsType<MidoriType::StructType>());

	const MidoriType::StructType& pair_type = pair_definition->m_annotated_type.value()->GetType<MidoriType::StructType>();
	REQUIRE(pair_type.m_name == "Pair");
	REQUIRE(pair_type.m_member_types.size() == 2u);
	REQUIRE(pair_type.m_member_types[0u]->IsType<MidoriType::IntegerType>());
	REQUIRE(pair_type.m_member_types[1u]->IsType<MidoriType::IntegerType>());
	REQUIRE(pair_definition->m_value->GetType()->IsType<MidoriType::StructType>());

	const MidoriType::StructType& constructed_pair_type = pair_definition->m_value->GetType()->GetType<MidoriType::StructType>();
	REQUIRE(constructed_pair_type.m_name == "Pair");
	REQUIRE(constructed_pair_type.m_member_types.size() == 2u);
	REQUIRE(constructed_pair_type.m_member_types[0u]->IsType<MidoriType::IntegerType>());
	REQUIRE(constructed_pair_type.m_member_types[1u]->IsType<MidoriType::IntegerType>());
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

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "ConstraintFailure.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeUnsatisfiedConstraint;
	expectation.m_message_substrings = { "Type Hidden does not satisfy constraint Show<Hidden>", "no matching instance found" };
	expectation.m_rendered_substrings = { "Type Checker Error", "ConstraintFailure.mmt:13", "Display(Hidden(1))" };
	RequireErrorMatches(typecheck_result.error(), expectation);
}

TEST_CASE("TypeChecker exposes failures through shared diagnostics transport", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module SharedDiagnostics
def number : Int = "oops";
)";

	std::expected<MidoriTest::ParsedSnippet, CompilerError> parse_result = MidoriTest::ParseSnippet(source_code, "SharedDiagnostics.mmt");
	REQUIRE(parse_result.has_value());

	MidoriResult::TypeCheckerResult typecheck_result = TypeChecker
	(
		std::move(parse_result->m_program),
		parse_result->m_source.FileName(),
		parse_result->m_source.SourceLines()
	).TypeCheck();

	REQUIRE_FALSE(typecheck_result.has_value());
	REQUIRE(typecheck_result.error().Size() == 1u);

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeMismatch;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Expected type 'Int' but got 'Text'" };
	expectation.m_rendered_substrings = { "Type Checker Error", "SharedDiagnostics.mmt:2" };
	RequireErrorMatches(typecheck_result.error().First(), expectation);
}

TEST_CASE("TypeChecker preserves multiple top-level failures as separate diagnostics", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module MultipleDiagnostics
def number : Int = "oops";
def flag : Bool = 123;
)";

	std::expected<MidoriTest::TypedSnippet, MidoriResult::CompilerDiagnostics> typecheck_result =
		MidoriTest::TypeCheckSnippetWithDiagnostics(source_code, "MultipleDiagnostics.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());
	REQUIRE(typecheck_result.error().Size() == 2u);

	MidoriTest::ErrorExpectation first_expectation;
	first_expectation.m_stage = CompilerStage::TypeChecker;
	first_expectation.m_code = CompilerErrorCode::TypeMismatch;
	first_expectation.m_line = 2;
	first_expectation.m_message_substrings = { "Expected type 'Int' but got 'Text'" };
	first_expectation.m_rendered_substrings = { "Type Checker Error", "MultipleDiagnostics.mmt:2" };
	RequireErrorMatches(typecheck_result.error().m_errors[0u], first_expectation);

	MidoriTest::ErrorExpectation second_expectation;
	second_expectation.m_stage = CompilerStage::TypeChecker;
	second_expectation.m_code = CompilerErrorCode::TypeMismatch;
	second_expectation.m_line = 3;
	second_expectation.m_message_substrings = { "Expected type 'Bool' but got 'Int'" };
	second_expectation.m_rendered_substrings = { "Type Checker Error", "MultipleDiagnostics.mmt:3" };
	RequireErrorMatches(typecheck_result.error().m_errors[1u], second_expectation);
}

TEST_CASE("Compiler tags undefined names with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module UndefinedName
def value = missing;
)";

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "UndefinedName.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::Parser;
	expectation.m_code = CompilerErrorCode::TypeUndefinedName;
	expectation.m_line = 2;
	expectation.m_message_substrings = { "Undefined name" };
	expectation.m_rendered_substrings = { "Parser Error", "UndefinedName.mmt:2" };
	RequireErrorMatches(typecheck_result.error(), expectation);
}

TEST_CASE("TypeChecker tags non-callable values with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module NotCallable
def value = 1;
def result = value();
)";

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "NotCallable.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeNotCallable;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "not a callable" };
	expectation.m_rendered_substrings = { "Type Checker Error", "NotCallable.mmt:3" };
	RequireErrorMatches(typecheck_result.error(), expectation);
}

TEST_CASE("TypeChecker tags incorrect arity with a stable diagnostic code", "[typechecker][diagnostics]")
{
	const std::string source_code =
		R"(module IncorrectArity
def id = fn(value: Int) -> Int => value;
def result = id();
)";

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "IncorrectArity.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeIncorrectArity;
	expectation.m_line = 3;
	expectation.m_message_substrings = { "incorrect arity" };
	expectation.m_rendered_substrings = { "Type Checker Error", "IncorrectArity.mmt:3" };
	RequireErrorMatches(typecheck_result.error(), expectation);
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

	std::expected<MidoriTest::TypedSnippet, CompilerError> typecheck_result = MidoriTest::TypeCheckSnippet(source_code, "NonExhaustiveMatch.mmt");
	REQUIRE_FALSE(typecheck_result.has_value());

	MidoriTest::ErrorExpectation expectation;
	expectation.m_stage = CompilerStage::TypeChecker;
	expectation.m_code = CompilerErrorCode::TypeNonExhaustiveMatch;
	expectation.m_line = 4;
	expectation.m_message_substrings = { "non-exhaustive match", "None" };
	expectation.m_rendered_substrings = { "Type Checker Error", "NonExhaustiveMatch.mmt:4" };
	RequireErrorMatches(typecheck_result.error(), expectation);
}
