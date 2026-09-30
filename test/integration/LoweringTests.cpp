#include <catch2/catch_test_macros.hpp>

#include "Utility/TestMode/TestMode.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/MidoriIR/Printer/MidoriIRPrinter.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"
#include "Utility/Driver/MidoriDriver.h"
#include "support/CompileHelpers.h"
#include "support/DiagnosticMatchers.h"
#include "support/TempProject.h"

#include <filesystem>
#include <string>
#include <vector>

namespace
{
	LoweredModule RequireLowered(std::string source)
	{
		std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::move(source));
		if (!lowered.has_value())
		{
			FAIL(lowered.error().Rendered());
		}
		const std::vector<MidoriIRViolation> violations = MidoriIRVerifier(lowered->m_module).Verify();
		for (const MidoriIRViolation& violation : violations)
		{
			UNSCOPED_INFO(violation.ToString());
		}
		REQUIRE(violations.empty());
		return std::move(lowered).value();
	}

	CompilerError RequireLoweringError(std::string source)
	{
		std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::move(source));
		REQUIRE_FALSE(lowered.has_value());
		REQUIRE(lowered.error().Size() == 1u);
		return lowered.error().m_errors.front();
	}

	void RequireErrorMatches(const CompilerError& error, const MidoriTest::ErrorExpectation& expectation)
	{
		std::string mismatch;
		const bool matched = MidoriTest::Matches(error, expectation, &mismatch);
		CAPTURE(mismatch);
		REQUIRE(matched);
	}
}

TEST_CASE("A program of scalars, branches and tail calls prints what it computes", "[midori_ir][lowering][backend]")
{
	const std::expected<MidoriTest::ExecutedSnippet, CompilerError> executed = MidoriTest::ExecuteSnippet(R"(module Parity
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
def Fib = fn(n: Int, a: Int, b: Int) -> Int => if n == 0 then a else Fib(n - 1, b, a + b);
def Show = fn(label: Text, value: Int) -> Unit => Print(label ++ "=" ++ (value as Text) ++ "\n");
def Pick = fn(flag: Bool) -> Text => if flag then "yes" else "no";
Show("fib", Fib(50, 0, 1));
Show("mixed", {
    def x = Fib(10, 0, 1);
    def y = if (x > 50) || (x < 0) then x * 2 else x / 2;
    y - (x % 7)
});
Print(Pick((Fib(5, 0, 1) == 5) && !(Fib(6, 0, 1) == 5)) ++ "\n");
Print(((Fib(7, 0, 1) as Float) / 2.0) as Text);
)");
	REQUIRE(executed.has_value());
	CHECK(executed->m_exit_code == 0);
	CHECK(executed->m_output.m_stdout == "fib=12586269025\nmixed=104\nyes\n6.5");
}
