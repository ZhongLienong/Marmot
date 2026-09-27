#include <catch2/catch_test_macros.hpp>

#include "Common/Constant/Constant.h"
#include "Common/Executable/Executable.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/MidoriIR/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/MidoriIRVerifier.h"
#include "Loader/ProgramLoader.h"
#include "support/CompileHelpers.h"
#include "support/OutputCapture.h"

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
