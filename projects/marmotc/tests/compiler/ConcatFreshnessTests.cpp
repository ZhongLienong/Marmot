#include <cstddef>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "support/CompileHelpers.h"

namespace
{
	// Pins where the backend may extend text in place. `++` on a left operand
	// nothing else can see is EXTEND_TEXT; on a name, which other code can
	// still read, or on a text literal, which every load shares, it is
	// CONCAT_TEXT. All go through the MidoriIR optimizer, because
	// test/prelude/success/concat_does_not_mutate_aliases.mmt and
	// test/text/literal_left_of_concat.mmt prove the same property only by
	// their output: if a pass folded their probes away, they would keep
	// passing while they stopped testing anything.

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

	// The right operand is read through an array index so nothing that
	// inspects only the AST's leaves can mistake it for a literal.
	const std::string LITERAL_LEFT_SOURCE =
		"module LiteralLeftProbe\n"
		"\n"
		"def src : Array<Text> = [\"y\"];\n"
		"def rt = src[0];\n"
		"def result = \"x\" ++ rt;\n";

	// The inner `++` is fresh, so the outer one may extend it in place.
	const std::string CHAIN_SOURCE =
		"module ChainProbe\n"
		"\n"
		"def src : Array<Text> = [\"y\"];\n"
		"def rt = src[0];\n"
		"def result = rt ++ rt ++ rt;\n";

	// SCCP folds the inner `++` into a text constant, which the outer one
	// must then not extend.
	const std::string FOLDED_CHAIN_SOURCE =
		"module FoldedChainProbe\n"
		"\n"
		"def src : Array<Text> = [\"z\"];\n"
		"def rt = src[0];\n"
		"def result = \"x\" ++ \"y\" ++ rt;\n";

	// The Case 1 shape: the left operand is a name bound earlier, never a
	// literal, so it must not be extended in place.
	const std::string CONCAT_SHAPE_SOURCE =
		"module ConcatTextProbe\n"
		"\n"
		"def a = \"x\";\n"
		"def result = a ++ \"y\";\n";
}

TEST_CASE("A text literal left operand of ++ emits CONCAT_TEXT and never EXTEND_TEXT", "[compiler][backend][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(LITERAL_LEFT_SOURCE, "LiteralLeftProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::CONCAT_TEXT));
	REQUIRE_FALSE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}

TEST_CASE("A ++ on the result of another ++ emits EXTEND_TEXT", "[compiler][backend][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(CHAIN_SOURCE, "ChainProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
}

TEST_CASE("A ++ on a folded text constant emits CONCAT_TEXT and never EXTEND_TEXT", "[compiler][backend][concat]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> module_result =
		MidoriTest::GenerateBytecodeSnippetWithDiagnostics(FOLDED_CHAIN_SOURCE, "FoldedChainProbe.mmt");
	REQUIRE(module_result.has_value());

	const BytecodeStream& main_procedure = MainProcedureOrFail(module_result.value());
	REQUIRE(ContainsOpCode(main_procedure, OpCode::CONCAT_TEXT));
	REQUIRE_FALSE(ContainsOpCode(main_procedure, OpCode::EXTEND_TEXT));
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
