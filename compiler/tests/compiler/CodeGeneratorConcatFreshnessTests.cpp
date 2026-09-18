#include <cstddef>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Common/Constant/Constant.h"
#include "Common/Executable/Executable.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "support/CompileHelpers.h"

namespace
{
	// Pins two different things about the EXTEND_TEXT in-place optimisation,
	// using two different helpers, because neither alone covers both:
	//
	//   - The first two TEST_CASEs use GenerateBytecodeSnippetWithDiagnostics,
	//     which calls the type checker and CodeGenerator directly and never
	//     runs OptimizerManager. They pin CodeGenerator::IsFreshConcatTemporary
	//     itself: a text-literal left operand of `++` must emit EXTEND_TEXT,
	//     and a NameAccess left operand must emit CONCAT_TEXT and never
	//     EXTEND_TEXT. This is the classifier's accept/reject behaviour on the
	//     raw AST shape, independent of whatever the optimizer does.
	//
	//   - The third TEST_CASE uses GenerateOptimizedBytecodeSnippetWithDiagnostics,
	//     which additionally runs OptimizerManager the way the real compiler
	//     does, and asserts EXTEND_TEXT is STILL present afterwards. This
	//     guards a different risk: test/prelude/success/concat_does_not_mutate_aliases.mmt
	//     proves the same property only by observing *behaviour* through the
	//     full compiler. If a future pass learned to fold an array index into
	//     a literal - or a future constant-propagation pass started covering
	//     module-level globals, which LocalConstantPropagation does not today
	//     (it is scoped to NameContext::Local) - several of that file's probes
	//     would stop reaching EXTEND_TEXT at all while still printing the
	//     same, correct output. The .mmt test would keep passing and nobody
	//     would notice it had stopped testing anything. The first two
	//     TEST_CASEs would also stay green in that scenario, because they never
	//     run the optimizer that would have done the folding - only the third
	//     one runs the real pipeline end to end and would go red. This is
	//     exactly the failure mode this branch has hit before: a benchmark
	//     that timed a stack overflow, and the vacuous literal-only Text
	//     Case 6 found in review of this test's first commit.

	[[nodiscard]] std::optional<std::size_t> FindMainProcedureIndex(const BytecodeModule& module)
	{
		for (std::size_t index = 0u; index < module.m_procedure_names.size(); index += 1u)
		{
			if (module.m_procedure_names[index].starts_with(MAIN_PROCEDURE_PREFIX))
			{
				return index;
			}
		}

		return std::nullopt;
	}

	// Walks every byte of the procedure and compares it to `target`. This is a
	// raw byte scan, not a decoded instruction walk: an operand byte that
	// happens to equal `target`'s ordinal would read as a match. The snippets
	// below are small enough that their operands are small indices unlikely to
	// collide with EXTEND_TEXT/CONCAT_TEXT's ordinals in practice, and a
	// correct decoded walk would mean duplicating a large part of
	// Disassembler.cpp's per-opcode operand-width switch, which is not worth
	// it here.
	[[nodiscard]] bool ContainsOpCode(const BytecodeStream& procedure, OpCode target)
	{
		for (BytecodeStream::const_iterator it = procedure.cbegin(); it != procedure.cend(); ++it)
		{
			if (*it == target)
			{
				return true;
			}
		}

		return false;
	}

	[[nodiscard]] const BytecodeStream& MainProcedureOrFail(const BytecodeModule& module)
	{
		const std::optional<std::size_t> main_index = FindMainProcedureIndex(module);
		if (!main_index.has_value())
		{
			FAIL("Compiled module has no $main$ procedure.");
		}

		return module.m_procedures[main_index.value()];
	}

	// The Case 2/6 shape: a fresh text literal on the left, and a right
	// operand read through an array index so it cannot be mistaken for a
	// second literal by anything that inspects only the AST's leaves.
	const std::string EXTEND_SHAPE_SOURCE =
		"module ExtendTextProbe\n"
		"\n"
		"def src : Array<Text> = [\"y\"];\n"
		"def rt = src[0];\n"
		"def result = \"x\" ++ rt;\n";

	// The Case 1 shape: the left operand is a NameAccess to an already-bound
	// binding, never a literal, so IsFreshConcatTemporary must reject it.
	const std::string CONCAT_SHAPE_SOURCE =
		"module ConcatTextProbe\n"
		"\n"
		"def a = \"x\";\n"
		"def result = a ++ \"y\";\n";
}

TEST_CASE("A text literal left operand of ++ emits EXTEND_TEXT", "[compiler][codegen][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(EXTEND_SHAPE_SOURCE, "ExtendTextProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}

TEST_CASE("A NameAccess left operand of ++ emits CONCAT_TEXT and never EXTEND_TEXT", "[compiler][codegen][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(CONCAT_SHAPE_SOURCE, "ConcatTextProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::CONCAT_TEXT));
	REQUIRE_FALSE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}

TEST_CASE("EXTEND_TEXT survives the real optimizer pipeline on the Case 2/6 shape", "[compiler][codegen][concat][optimizer]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateOptimizedBytecodeSnippetWithDiagnostics(EXTEND_SHAPE_SOURCE, "ExtendTextProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}
