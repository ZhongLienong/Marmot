#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/MarmotIR/Builder/MarmotIRBuilder.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "support/CompileHelpers.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <vector>

namespace
{
	using TypeRef = std::shared_ptr<MarmotType>;

	const TypeRef& ScalarType(MarmotIRScalar scalar)
	{
		return MarmotIRScalarType(scalar);
	}

	BytecodeModule RequireBytecode(const LoweredModule& lowered)
	{
		const std::vector<std::string> source_lines;
		MarmotResult::BytecodeBackendResult bytecode = BytecodeBackend(lowered, "Test.mmt", source_lines).Emit();
		if (!bytecode.HasValue())
		{
			FAIL(bytecode.Error().Rendered());
		}
		return std::move(bytecode).Value();
	}

	std::vector<OpCode> Opcodes(const BytecodeStream& procedure)
	{
		std::vector<OpCode> opcodes;
		for (int offset = 0; offset < procedure.GetByteCodeSize(); offset += OpCodeTable::Length(procedure.ReadByteCode(offset)))
		{
			opcodes.push_back(procedure.ReadByteCode(offset));
		}
		return opcodes;
	}


}

TEST_CASE("The backend leaves a value used once, where it is made, on the stack", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<LoweredModule> lowered = MarmotTest::LowerSnippetWithDiagnostics(R"(module Stack
def Square = fn(n: Int) -> Int => (n * n) + 1;
)");
	REQUIRE(lowered.HasValue());

	const BytecodeModule bytecode = RequireBytecode(lowered.Value());
	REQUIRE(bytecode.m_procedure_names[1u] == "Square@Stack");
	CHECK(Opcodes(bytecode.m_procedures[1u]) == std::vector<OpCode>
	{
		OpCode::GET_LOCAL2,
		OpCode::MULTIPLY_INTEGER,
		OpCode::INT_1,
		OpCode::ADD_INTEGER,
		OpCode::RETURN
	});
}

TEST_CASE("The backend jumps on a comparison without making its Bool", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<LoweredModule> lowered = MarmotTest::LowerSnippetWithDiagnostics(R"(module Fused
def Fib = fn(n: Int) -> Int => if n <= 1 then n else Fib(n - 1) + Fib(n - 2);
def Order = fn(a: Float, b: Float) -> Int => if a < b then 0 else 1;
)");
	REQUIRE(lowered.HasValue());

	const BytecodeModule bytecode = RequireBytecode(lowered.Value());
	CHECK(Opcodes(bytecode.m_procedures[1u]) == std::vector<OpCode>
	{
		OpCode::IF_LOCAL_LE_INT,
		OpCode::GET_LOCAL,
		OpCode::RETURN,
		OpCode::PUSH_LOCAL_SUB_INT,
		OpCode::CALL_PROC_WIDE,
		OpCode::PUSH_LOCAL_SUB_INT,
		OpCode::CALL_PROC_WIDE,
		OpCode::ADD_INTEGER,
		OpCode::RETURN
	});
	CHECK(Opcodes(bytecode.m_procedures[2u]) == std::vector<OpCode>
	{
		OpCode::GET_LOCAL2,
		OpCode::IF_FLOAT_LESS,
		OpCode::INT_0,
		OpCode::RETURN,
		OpCode::INT_1,
		OpCode::RETURN
	});
}

TEST_CASE("Two values in slots that are never live at once share one", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<LoweredModule> lowered = MarmotTest::LowerSnippetWithDiagnostics(R"(module Share
def Twice = fn(n: Int) -> Int => {
	def a = n * 3;
	def b = a + a;
	def c = b * 5;
	c + c
};
)");
	REQUIRE(lowered.HasValue());

	// n is dead once a is made, and a once b is, so a, b and c take n's slot
	// in turn and the frame needs no slot beyond its parameter.
	const BytecodeModule bytecode = RequireBytecode(lowered.Value());
	const std::vector<OpCode> opcodes = Opcodes(bytecode.m_procedures[1u]);
	CHECK(std::ranges::count(opcodes, OpCode::PUSH_PLACEHOLDER) == 0);
}

TEST_CASE("The backend makes the top-level function procedure 0 and names each procedure for its module", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<LoweredModule> lowered = MarmotTest::LowerSnippetWithDiagnostics(R"(module Names
def One = fn() -> Int => 1;
def two = One() + 1;
)");
	REQUIRE(lowered.HasValue());

	const BytecodeModule bytecode = RequireBytecode(lowered.Value());
	CHECK(bytecode.m_procedure_names == std::vector<std::string>{ "$main$@Names", "One@Names" });
	CHECK(bytecode.m_global_variables == std::vector<std::string>{ "One", "two" });
	CHECK(Opcodes(bytecode.m_procedures[0u]).back() == OpCode::RETURN);
}

TEST_CASE("Linked procedures list the globals read through recursive calls and new closures", "[marmot_ir][backend][linker]")
{
	std::string source = "module References\n";
	for (int index = 0; index < 258; index += 1)
	{
		source += std::format("def padding{} = 0;\n", index);
	}
	source += R"(
def left = 12;
def right = 5;
def Even = fn(n: Int) -> Int => if n == 0 then right + left + left else Odd(n - 1) + 1;
def Odd = fn(n: Int) -> Int => if n == 0 then 0 else Even(n - 1) + 1;
def MakeReader = fn(n: Int) -> fn() -> Int => fn() -> Int => n + left;
)";
	MarmotResult::DiagnosticsResult<LoweredModule> lowered = MarmotTest::LowerSnippetWithDiagnostics(std::move(source));
	REQUIRE(lowered.HasValue());
	std::vector<BytecodeModule> modules;
	modules.push_back(RequireBytecode(lowered.Value()));
	MarmotResult::BytecodeLinkerResult result = BytecodeLinker(std::move(modules), "References").Link();
	REQUIRE(result.HasValue());
	const MarmotExecutable& executable = result.Value();
	REQUIRE(executable.GetGlobalVariable(258) == "left");
	REQUIRE(executable.GetGlobalVariable(259) == "right");

	for (const std::string_view name : { "Even@References", "Odd@References", "MakeReader@References" })
	{
		CAPTURE(name);
		const std::vector<std::string>::const_iterator procedure_name = std::ranges::find(executable.m_procedure_names, name);
		REQUIRE(procedure_name != executable.m_procedure_names.end());
		const BytecodeStream& procedure = executable.GetBytecodeStream(static_cast<int>(procedure_name - executable.m_procedure_names.begin()));
		if (name == "MakeReader@References")
		{
			CHECK(procedure.GetReferencedGlobals() == std::vector<uint16_t>{ 258u });
		}
		else
		{
			CHECK(procedure.GetReferencedGlobals() == std::vector<uint16_t>{ 258u, 259u });
		}
		if (name == "Even@References")
		{
			std::vector<uint8_t> global_operands;
			for (int offset = 0; offset < procedure.GetByteCodeSize(); offset += OpCodeTable::Length(procedure.ReadByteCode(offset)))
			{
				if (procedure.ReadByteCode(offset) == OpCode::GET_GLOBAL_WIDE)
				{
					global_operands.push_back(static_cast<uint8_t>(procedure.ReadByteCode(offset + 1)));
					global_operands.push_back(static_cast<uint8_t>(procedure.ReadByteCode(offset + 2)));
				}
			}
			CHECK(global_operands == std::vector<uint8_t>{ 3u, 1u, 2u, 1u, 2u, 1u });
		}
	}
}

// Built by hand, so its shape is exactly this: each iteration
// passes a block's two parameters back to it swapped, which a jump that
// stored the first before reading the second would get wrong.
TEST_CASE("The backend reads a union's tag and fields straight from its slot in a loop", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode = MarmotTest::GenerateBytecodeSnippetWithDiagnostics(R"(module Walk
type Chain = End | Link(Int, Chain);
def Sum = fn(chain: Chain, total: Int) -> Int => match chain with
	case Chain::Link(value, rest) => Sum(rest, total + value)
	case Chain::End() => total;
)");
	REQUIRE(bytecode.HasValue());

	// The match's last arm needs no test of its own, so the tag is read once,
	// from the slot, and not kept.
	REQUIRE(bytecode->m_procedure_names[1u] == "Sum@Walk");
	CHECK(Opcodes(bytecode->m_procedures[1u]) == std::vector<OpCode>
	{
		OpCode::PUSH_PLACEHOLDER,
		OpCode::IF_LOCAL_TAG_NOT,
		OpCode::LOCAL_UNION_FIELD,
		OpCode::GET_LOCAL,
		OpCode::GET_UNION_FIELD,
		OpCode::GET_LOCAL2,
		OpCode::ADD_INTEGER,
		OpCode::STORE_LOCAL,
		OpCode::STORE_LOCAL,
		OpCode::JUMP_BACK,
		OpCode::GET_LOCAL,
		OpCode::RETURN
	});
}

TEST_CASE("The backend compares, steps and builds from slots in a loop without the stack", "[marmot_ir][backend]")
{
	MarmotResult::DiagnosticsResult<BytecodeModule> bytecode = MarmotTest::GenerateBytecodeSnippetWithDiagnostics(R"(module Split
type Chain = End | Link(Int, Chain);
def Keep = fn(values: Array<Int>, index: Int, pivot: Int, below: Chain) -> Chain =>
	if index < 0
	then below
	else
	{
		def value = values[index];
		if value < pivot
		then Keep(values, index - 1, pivot, Chain::Link(value, below))
		else if value == pivot
		then Keep(values, index - 1, pivot, below)
		else Keep(values, index - 1, pivot, Chain::Link(value * 2, below))
	};
def Doubles = fn(n: Int) -> Array<Int> => [i * 2 for i in 0..1..n];
)");
	REQUIRE(bytecode.HasValue());

	// A union made of two locals is one instruction; one made of a value
	// computed on the stack is built there.
	REQUIRE(bytecode->m_procedure_names[1u] == "Keep@Split");
	CHECK(Opcodes(bytecode->m_procedures[1u]) == std::vector<OpCode>
	{
		OpCode::PUSH_PLACEHOLDER,
		OpCode::IF_LOCAL_LT_INT,
		OpCode::GET_LOCAL,
		OpCode::RETURN,
		OpCode::LOCAL_ARRAY_GET,
		OpCode::IF_LOCAL_LT_LOCAL,
		OpCode::STEP_LOCAL,
		OpCode::LOCAL_UNION2,
		OpCode::JUMP_BACK,
		OpCode::IF_LOCAL_EQ_LOCAL,
		OpCode::STEP_LOCAL,
		OpCode::JUMP_BACK,
		OpCode::STEP_LOCAL,
		OpCode::GET_LOCAL,
		OpCode::INT_1,
		OpCode::LEFT_SHIFT,
		OpCode::GET_LOCAL,
		OpCode::CONSTRUCT_UNION,
		OpCode::STORE_LOCAL,
		OpCode::JUMP_BACK
	});
	REQUIRE(bytecode->m_procedure_names[2u] == "Doubles@Split");
	CHECK(Opcodes(bytecode->m_procedures[2u]) == std::vector<OpCode>
	{
		OpCode::PUSH_PLACEHOLDER,
		OpCode::CREATE_ARRAY,
		OpCode::STORE_LOCAL,
		OpCode::INT_0,
		OpCode::STORE_LOCAL,
		OpCode::IF_LOCAL_LT_LOCAL,
		OpCode::GET_LOCAL,
		OpCode::INT_1,
		OpCode::LEFT_SHIFT,
		OpCode::STORE_LOCAL,
		OpCode::APPEND_LOCAL,
		OpCode::STEP_LOCAL,
		OpCode::JUMP_BACK,
		OpCode::GET_LOCAL,
		OpCode::RETURN
	});
}
