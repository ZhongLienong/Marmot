#include <catch2/catch_test_macros.hpp>

#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"
#include "support/CompileHelpers.h"

namespace
{
	MarmotExpression::Function& FunctionIn(MarmotTest::ParsedSnippet& parsed)
	{
		REQUIRE(parsed.m_program.size() == 1u);
		MarmotStatement::VariableDefinition& definition = parsed.m_program[0]->GetStatement<MarmotStatement::VariableDefinition>();
		return definition.m_value->GetExpression<MarmotExpression::Function>();
	}

	void RequireCheckedAndLowered(MarmotTest::ParsedSnippet& parsed)
	{
		MarmotResult::TypeCheckerResult checked = TypeChecker(std::move(parsed.m_program), parsed.m_source.FileName(), parsed.m_source.SourceLines()).TypeCheck();
		if (!checked.HasValue())
		{
			FAIL(checked.Error().Rendered());
		}
		const std::vector<CompilerWarning> warnings = StaticAnalyzerManager({}).Analyze(checked.Value(), parsed.m_source.FileName(), parsed.m_source.SourceLines());
		CHECK(warnings.empty());
		MarmotResult::DiagnosticsResult<LoweredModule> lowered = Lowering(checked.Value(), parsed.m_source.FileName(), parsed.m_source.SourceLines(), "GeneratedBindings", {}, {}).Lower();
		if (!lowered.HasValue())
		{
			FAIL(lowered.Error().Rendered());
		}
		CHECK(MarmotIRVerifier(lowered->m_module).Verify().empty());
	}
}

TEST_CASE("Generated parameters resolve by identity through checking and lowering", "[typechecker][generated-bindings]")
{
	MarmotResult::Result<MarmotTest::ParsedSnippet> parsed = MarmotTest::ParseSnippet("module GeneratedBindings\ndef choose = fn(left: Int, right: Text) -> Text => right;\n", "GeneratedBindings.mmt");
	if (!parsed.HasValue())
	{
		FAIL(parsed.Error().Rendered());
	}
	MarmotExpression::Function& function = FunctionIn(*parsed);
	const GeneratedBinding left(7, function.m_function_keyword, function.m_params.Function());
	const GeneratedBinding right(19, function.m_function_keyword, function.m_params.Function());
	function.m_params = FunctionParameters::Generated({ left, right }, function.m_params.Function());
	function.m_body = std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(right));
	CHECK(function.m_params.SourceToken(0).m_lexeme == function.m_params.SourceToken(1).m_lexeme);
	CHECK(function.m_params.DisplayName(0) != function.m_params.DisplayName(1));
	function.m_body->GetExpression<MarmotExpression::NameAccess>().m_name.m_lexeme = "unbound-diagnostic-text";
	RequireCheckedAndLowered(*parsed);
}

TEST_CASE("Generated pattern bindings use identities independent of source tokens", "[typechecker][generated-bindings]")
{
	MarmotResult::Result<MarmotTest::ParsedSnippet> parsed = MarmotTest::ParseSnippet("module GeneratedBindings\ndef choose = fn(input: (Int, Text)) -> Text => match input with case (left, right) => right;\n", "GeneratedBindings.mmt");
	if (!parsed.HasValue())
	{
		FAIL(parsed.Error().Rendered());
	}
	MarmotExpression::Function& function = FunctionIn(*parsed);
	const GeneratedBinding input(7, function.m_function_keyword, function.m_params.Function());
	const GeneratedBinding left(19, function.m_function_keyword, function.m_params.Function());
	const GeneratedBinding right(31, function.m_function_keyword, function.m_params.Function());
	function.m_params = FunctionParameters::Generated({ input }, function.m_params.Function());
	MarmotExpression::Match& match = function.m_body->GetExpression<MarmotExpression::Match>();
	match.m_arg_expr = std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(input));
	MarmotExpression::Case& arm = match.m_cases[0]->GetExpression<MarmotExpression::Case>();
	MarmotPattern::Tuple& pattern = arm.m_pattern->GetPattern<MarmotPattern::Tuple>();
	pattern.m_elements[0] = std::make_unique<MarmotPattern>(MarmotPattern::Binding(left));
	pattern.m_elements[1] = std::make_unique<MarmotPattern>(MarmotPattern::Binding(right));
	arm.m_expr = std::make_unique<MarmotExpression>(MarmotExpression::NameAccess(right));
	RequireCheckedAndLowered(*parsed);
}
