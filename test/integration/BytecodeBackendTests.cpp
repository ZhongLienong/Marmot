#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/MidoriIR/Builder/MidoriIRBuilder.h"
#include "Compiler/MidoriIR/Verifier/MidoriIRVerifier.h"
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

		const std::pair<int, MidoriTest::CapturedOutput> run = MidoriTest::RunExecutable(linked.value());
		REQUIRE(run.first == 0);
		return run.second.m_stdout;
	}
}

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
