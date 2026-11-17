#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "Compiler/BytecodeBackend/BytecodeBackend.h"
#include "Compiler/BytecodeLinker/BytecodeLinker.h"
#include "Compiler/MarmotIR/Builder/MarmotIRBuilder.h"
#include "Compiler/MarmotIR/Verifier/MarmotIRVerifier.h"
#include "support/CompileHelpers.h"
#include "support/OutputCapture.h"

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
		MarmotResult::BytecodeLinkerResult linked = BytecodeLinker(std::move(modules), name).Link();
		if (!linked.has_value())
		{
			FAIL(linked.error().Rendered());
		}

		const std::pair<int, MarmotTest::CapturedOutput> run = MarmotTest::RunExecutable(linked.value());
		REQUIRE(run.first == 0);
		return run.second.m_stdout;
	}
}

TEST_CASE("The backend moves a jump's arguments into its target's parameters all at once", "[marmot_ir][backend]")
{
	MarmotIRModule module("Swap");
	const MarmotIRGlobalSlot print = module.ReserveGlobal("Print", ScalarType(MarmotIRScalar::Text));
	MarmotIRFunction function(std::string(MAIN_PROCEDURE_PREFIX), MarmotType::MakeLiteralType<MarmotType::UnitType>());
	MarmotIRBuilder builder(function);
	const MarmotIRBlockId loop = builder.CreateBlock();
	const MarmotIRBlockId body = builder.CreateBlock();
	const MarmotIRBlockId done = builder.CreateBlock();

	builder.Emit(MarmotIROp::GlobalDefine, MarmotType::MakeLiteralType<MarmotType::UnitType>(), { builder.ConstText("MARMOT_FFI_Print") }, print);
	builder.Jump(loop, { builder.ConstText("a"), builder.ConstText("b"), builder.ConstInt(3) });

	const MarmotIRValueId first = builder.AddParameter(loop, ScalarType(MarmotIRScalar::Text), "first");
	const MarmotIRValueId second = builder.AddParameter(loop, ScalarType(MarmotIRScalar::Text), "second");
	const MarmotIRValueId count = builder.AddParameter(loop, ScalarType(MarmotIRScalar::Int), "count");
	builder.PositionAt(loop);
	builder.Branch(builder.Binary(MarmotIROp::EqInt, count, builder.ConstInt(0)), MarmotIRSuccessor(done), MarmotIRSuccessor(body));

	builder.PositionAt(body);
	const MarmotIRValueId pair = builder.Emit(MarmotIROp::Concat, ScalarType(MarmotIRScalar::Text), { first, second });
	builder.Emit(MarmotIROp::CallForeign, MarmotType::MakeLiteralType<MarmotType::UnitType>(), { builder.Emit(MarmotIROp::Concat, ScalarType(MarmotIRScalar::Text), { pair, builder.ConstText(" ") }) }, MarmotIRForeign{ "MARMOT_FFI_Print" });
	builder.Jump(loop, { second, first, builder.Binary(MarmotIROp::SubInt, count, builder.ConstInt(1)) });

	builder.PositionAt(done).Return(builder.ConstUnit());
	module.m_top_level = module.AddFunction(std::move(function));
	REQUIRE(MarmotIRVerifier(module).Verify().empty());

	const LoweredModule lowered(std::move(module));
	CHECK(RunModule(RequireBytecode(lowered)) == "ab ba ab ");
}

TEST_CASE("A frame of more than 255 slots pushes its placeholders in several instructions", "[marmot_ir][backend]")
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
	std::expected<LoweredModule, MarmotResult::CompilerDiagnostics> lowered = MarmotTest::LowerSnippetWithDiagnostics(std::format
	(
		R"(module Wide
foreign "MARMOT_FFI_Print" Print: fn(Text) -> Unit;
def Total = fn(n: Int) -> Int => {{
{}	{}
}};
Print(Total(1) as Text);
)",
		definitions,
		sum
	));
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
