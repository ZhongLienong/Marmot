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
	// Pins where the backend may extend text in place. `++` on a left operand
	// nothing else can see is EXTEND_TEXT; on a name, which other code can
	// still read, it is CONCAT_TEXT. Both go through the MidoriIR optimizer,
	// because test/prelude/success/concat_does_not_mutate_aliases.mmt proves
	// the same property only by its output: if a pass folded its probes away,
	// that test would keep passing while it stopped testing anything, and only
	// the first case here would go red.

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

	// Steps through the procedure instruction by instruction, so an operand byte
	// that happens to equal `target`'s ordinal is never mistaken for it.
	[[nodiscard]] bool ContainsOpCode(const BytecodeStream& procedure, OpCode target)
	{
		for (int offset = 0; offset < procedure.GetByteCodeSize();)
		{
			const OpCode opcode = procedure.ReadByteCode(offset);
			if (opcode == target)
			{
				return true;
			}

			const int length = OpCodeTable::Length(opcode);
			if (length == 0)
			{
				FAIL("Byte " + std::to_string(static_cast<int>(opcode)) + " at offset " + std::to_string(offset) + " is not an opcode.");
			}
			offset += length;
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

	// The Case 1 shape: the left operand is a name bound earlier, never a
	// literal, so it must not be extended in place.
	const std::string CONCAT_SHAPE_SOURCE =
		"module ConcatTextProbe\n"
		"\n"
		"def a = \"x\";\n"
		"def result = a ++ \"y\";\n";
}

TEST_CASE("A text literal left operand of ++ emits EXTEND_TEXT", "[compiler][backend][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(EXTEND_SHAPE_SOURCE, "ExtendTextProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}

TEST_CASE("A name as the left operand of ++ emits CONCAT_TEXT and never EXTEND_TEXT", "[compiler][backend][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(CONCAT_SHAPE_SOURCE, "ConcatTextProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::CONCAT_TEXT));
	REQUIRE_FALSE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}
