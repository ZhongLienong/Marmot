#include <catch2/catch_test_macros.hpp>

#include "Common/BuildConfig/BuildConfig.h"
#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/MidoriIR/Builder/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"
#include "Loader/ProgramLoader.h"
#include "support/CompileHelpers.h"
#include "support/OutputCapture.h"

#include <format>
#include <ranges>
#include <string>
#include <vector>

namespace
{
	using TypeRef = std::shared_ptr<MidoriType>;

	const TypeRef& ScalarType(MidoriIRScalar scalar)
	{
		return MidoriIRScalarType(scalar);
	}

	BytecodeModule RequireBytecode(const LoweredModule& lowered)
	{
		const std::vector<std::string> source_lines;
		MidoriResult::BytecodeBackendResult bytecode = BytecodeBackend(lowered, "Test.mmt", source_lines).Emit();
		if (!bytecode.has_value())
		{
			FAIL(bytecode.error().Rendered());
		}
		return std::move(bytecode).value();
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

	// Links one module on its own and runs it, as the VM would.
	std::string RunModule(BytecodeModule&& module)
	{
		const std::string name = module.m_module_name;
		std::vector<BytecodeModule> modules;
		modules.push_back(std::move(module));
		MidoriResult::BytecodeLinkerResult linked = BytecodeLinker(std::move(modules), name).Link();
		if (!linked.has_value())
		{
			FAIL(linked.error().Rendered());
		}

		const MidoriBuild::ScopedTestModeOverride test_mode_override(true);
		MidoriTest::OutputCapture capture;
		std::expected<int, RuntimeError> run = MidoriProgramLoader::Run(std::move(linked).value());
		const MidoriTest::CapturedOutput output = capture.Stop();
		REQUIRE(run.has_value());
		REQUIRE(run.value() == 0);
		return output.m_stdout;
	}
}

TEST_CASE("The backend leaves a value used once, where it is made, on the stack", "[midori_ir][backend]")
{
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(R"(module Stack
def Square = fn(n: Int) -> Int => (n * n) + 1;
)");
	REQUIRE(lowered.has_value());

	const BytecodeModule bytecode = RequireBytecode(lowered.value());
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

TEST_CASE("The backend jumps on a comparison without making its Bool", "[midori_ir][backend]")
{
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(R"(module Fused
def Fib = fn(n: Int) -> Int => if n <= 1 then n else Fib(n - 1) + Fib(n - 2);
def Order = fn(a: Float, b: Float) -> Int => if a < b then 0 else 1;
)");
	REQUIRE(lowered.has_value());

	const BytecodeModule bytecode = RequireBytecode(lowered.value());
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

TEST_CASE("Two values in slots that are never live at once share one", "[midori_ir][backend]")
{
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(R"(module Share
def Twice = fn(n: Int) -> Int => {
	def a = n * 3;
	def b = a + a;
	def c = b * 5;
	c + c
};
)");
	REQUIRE(lowered.has_value());

	// n is dead once a is made, and a once b is, so a, b and c take n's slot
	// in turn and the frame needs no slot beyond its parameter.
	const BytecodeModule bytecode = RequireBytecode(lowered.value());
	const std::vector<OpCode> opcodes = Opcodes(bytecode.m_procedures[1u]);
	CHECK(std::ranges::count(opcodes, OpCode::PUSH_PLACEHOLDER) == 0);
}

TEST_CASE("The backend makes the top-level function procedure 0 and names each procedure for its module", "[midori_ir][backend]")
{
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(R"(module Names
def One = fn() -> Int => 1;
def two = One() + 1;
)");
	REQUIRE(lowered.has_value());

	const BytecodeModule bytecode = RequireBytecode(lowered.value());
	CHECK(bytecode.m_procedure_names == std::vector<std::string>{ "$main$@Names", "One@Names" });
	CHECK(bytecode.m_global_variables == std::vector<std::string>{ "One", "two" });
	CHECK(Opcodes(bytecode.m_procedures[0u]).back() == OpCode::RETURN);
}

// Built by hand, so its shape is exactly this: each iteration
// passes a block's two parameters back to it swapped, which a jump that
// stored the first before reading the second would get wrong.
TEST_CASE("The backend moves a jump's arguments into its target's parameters all at once", "[midori_ir][backend]")
{
	MidoriIRModule module("Swap");
	const MidoriIRGlobalSlot print = module.ReserveGlobal("Print", ScalarType(MidoriIRScalar::Text));
	MidoriIRFunction function(std::string(MAIN_PROCEDURE_PREFIX), MidoriType::MakeLiteralType<MidoriType::UnitType>());
	MidoriIRBuilder builder(function);
	const MidoriIRBlockId loop = builder.CreateBlock();
	const MidoriIRBlockId body = builder.CreateBlock();
	const MidoriIRBlockId done = builder.CreateBlock();

	builder.Emit(MidoriIROp::GlobalDefine, MidoriType::MakeLiteralType<MidoriType::UnitType>(), { builder.ConstText("MIDORI_FFI_Print") }, print);
	builder.Jump(loop, { builder.ConstText("a"), builder.ConstText("b"), builder.ConstInt(3) });

	const MidoriIRValueId first = builder.AddParameter(loop, ScalarType(MidoriIRScalar::Text), "first");
	const MidoriIRValueId second = builder.AddParameter(loop, ScalarType(MidoriIRScalar::Text), "second");
	const MidoriIRValueId count = builder.AddParameter(loop, ScalarType(MidoriIRScalar::Int), "count");
	builder.PositionAt(loop);
	builder.Branch(builder.Binary(MidoriIROp::EqInt, count, builder.ConstInt(0)), MidoriIRSuccessor(done), MidoriIRSuccessor(body));

	builder.PositionAt(body);
	const MidoriIRValueId pair = builder.Emit(MidoriIROp::Concat, ScalarType(MidoriIRScalar::Text), { first, second });
	builder.Emit(MidoriIROp::CallForeign, MidoriType::MakeLiteralType<MidoriType::UnitType>(), { builder.Emit(MidoriIROp::Concat, ScalarType(MidoriIRScalar::Text), { pair, builder.ConstText(" ") }) }, MidoriIRForeign{ "MIDORI_FFI_Print" });
	builder.Jump(loop, { second, first, builder.Binary(MidoriIROp::SubInt, count, builder.ConstInt(1)) });

	builder.PositionAt(done).Return(builder.ConstUnit());
	module.m_top_level = module.AddFunction(std::move(function));
	REQUIRE(MidoriIRVerifier(module).Verify().empty());

	const LoweredModule lowered(std::move(module));
	CHECK(RunModule(RequireBytecode(lowered)) == "ab ba ab ");
}

TEST_CASE("The backend reads a union's tag and fields straight from its slot in a loop", "[midori_ir][backend]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode = MidoriTest::GenerateBytecodeSnippetWithDiagnostics(R"(module Walk
type Chain = End | Link(Int, Chain);
def Sum = fn(chain: Chain, total: Int) -> Int => match chain with
	case Chain::Link(value, rest) => Sum(rest, total + value)
	case Chain::End => total;
)");
	REQUIRE(bytecode.has_value());

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

TEST_CASE("The backend compares, steps and builds from slots in a loop without the stack", "[midori_ir][backend]")
{
	std::expected<BytecodeModule, MidoriResult::CompilerDiagnostics> bytecode = MidoriTest::GenerateBytecodeSnippetWithDiagnostics(R"(module Split
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
	REQUIRE(bytecode.has_value());

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

TEST_CASE("A frame of more than 255 slots pushes its placeholders in several instructions", "[midori_ir][backend]")
{
	constexpr int s_count = 300;
	const std::string definitions = std::views::iota(0, s_count)
		| std::views::transform([](int index) { return std::format("\tdef v{} = n + {};\n", index, index); })
		| std::views::join
		| std::ranges::to<std::string>();
	const std::string sum = std::views::iota(0, s_count)
		| std::views::transform([](int index) { return std::format("v{}", index); })
		| std::views::join_with(std::string_view(" + "))
		| std::ranges::to<std::string>();
	std::expected<LoweredModule, MidoriResult::CompilerDiagnostics> lowered = MidoriTest::LowerSnippetWithDiagnostics(std::format(R"(module Wide
foreign "MIDORI_FFI_Print" Print: fn(Text) -> Unit;
def Total = fn(n: Int) -> Int => {{
{}	{}
}};
Print(Total(1) as Text);
)", definitions, sum));
	REQUIRE(lowered.has_value());

	BytecodeModule bytecode = RequireBytecode(lowered.value());
	REQUIRE(bytecode.m_procedure_names[1u] == "Total@Wide");
	const BytecodeStream& total = bytecode.m_procedures[1u];
	REQUIRE(total.ReadByteCode(0) == OpCode::PUSH_PLACEHOLDER);
	CHECK(static_cast<int>(total.ReadByteCode(1)) == 255);
	REQUIRE(total.ReadByteCode(2) == OpCode::PUSH_PLACEHOLDER);
	CHECK(static_cast<int>(total.ReadByteCode(3)) > 0);
	CHECK(RunModule(std::move(bytecode)) == std::to_string(s_count + s_count * (s_count - 1) / 2));
}
