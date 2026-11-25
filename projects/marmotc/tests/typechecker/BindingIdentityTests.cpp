#include <catch2/catch_test_macros.hpp>

#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "Compiler/StaticAnalyzerManager/StaticAnalyzerManager.h"
#include "Compiler/TypeChecker/TypeChecker.h"
#include "support/CompileHelpers.h"

namespace
{
	class RenameReferences final : public MarmotAbstractSyntaxTreeWalker
	{
	public:
		void Apply(MarmotProgramTree& program)
		{
			VisitProgram(program);
		}

	protected:
		void operator()(MarmotExpression::NameAccess& name) override
		{
			if (!std::holds_alternative<MarmotExpression::NameContext::Global>(name.m_name_ctx))
			{
				name.m_name.m_lexeme = "diagnostic-text";
			}
		}
	};
}

TEST_CASE("Source binding references keep their meaning independently of diagnostic text", "[typechecker][binding-identity]")
{
	const std::vector<std::string> sources
	{
		R"(module BindingIdentity
def choose = fn(input: Text) -> Text => {
    def kept = input;
    (fn() -> Text => (fn() -> Text => kept)())()
};
)",
		R"(module BindingIdentity
def choose = fn(input: (Int, Text)) -> Text =>
    match input with case (_, text) => (fn() -> Text => text)();
)",
		R"(module BindingIdentity
def choose = fn(input: Text) -> Text => {
    def identity = fn<T>(value: T) -> T => value;
    def count = identity(1);
    identity(if count == 1 then input else "other")
};
)"
	};
	for (const std::string& source : sources)
	{
		INFO(source);
		MarmotResult::Result<MarmotTest::ParsedSnippet> parsed = MarmotTest::ParseSnippet(source);
		REQUIRE(parsed.HasValue());
		RenameReferences().Apply(parsed->m_program);
		MarmotResult::TypeCheckerResult checked = TypeChecker(std::move(parsed->m_program), parsed->m_source.FileName(), parsed->m_source.SourceLines()).TypeCheck();
		if (!checked.HasValue())
		{
			FAIL(checked.Error().Rendered());
		}
		CHECK(StaticAnalyzerManager({}).Analyze(checked.Value(), parsed->m_source.FileName(), parsed->m_source.SourceLines()).empty());
		MarmotResult::DiagnosticsResult<LoweredModule> lowered = Lowering(checked.Value(), parsed->m_source.FileName(), parsed->m_source.SourceLines(), "BindingIdentity", {}, {}).Lower();
		if (!lowered.HasValue())
		{
			FAIL(lowered.Error().Rendered());
		}
		CHECK(MarmotIRVerifier(lowered->m_module).Verify().empty());
	}
}
