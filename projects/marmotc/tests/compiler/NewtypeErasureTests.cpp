#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Format/Format.h"
#include "Bytecode/Executable/Executable.h"
#include "support/CompileHelpers.h"
#include "Support/Optional.h"

namespace
{
	// "Opcode selection erases. Dispatch stays nominal." A newtype must cost
	// nothing at runtime: its main procedure must be byte-identical to the
	// same arithmetic written directly against its representation. This is
	// the property recorded only as a commit-message opcode dump in a701ac2 -
	// the one test the design spec calls out as the one that must not be
	// skipped, because a regression that reintroduces boxing would otherwise
	// pass the entire suite.

	[[nodiscard]] MarmotExecutable CompileOrFail(std::string source)
	{
		MarmotResult::ExecutableDiagnosticsResult result = MarmotTest::CompileSnippet(std::move(source));
		if (!result.HasValue())
		{
			FAIL("Test snippet failed to compile.");
		}
		return std::move(result).Value();
	}

	[[nodiscard]] MarmotCompiler::Optional<int> FindMainProcedureIndex(const MarmotExecutable& executable)
	{
		for (size_t index = 0u; index < executable.m_procedure_names.size(); index += 1u)
		{
			if (std::string(executable.m_procedure_names[index]).starts_with(MmcWire::MAIN_PROCEDURE_PREFIX))
			{
				return static_cast<int>(index);
			}
		}

		return MarmotCompiler::None;
	}

	const std::string NEWTYPE_SOURCE =
		"module Main\n"
		"\n"
		"type Meters = Int;\n"
		"\n"
		"def main = fn() -> Int => {\n"
		"    def a: Meters = 3 as Meters;\n"
		"    def b: Meters = 4 as Meters;\n"
		"    def sum: Int = (a as Int) + (b as Int);\n"
		"    def diff: Int = (a as Int) - (b as Int);\n"
		"    sum * diff\n"
		"};\n";

	const std::string REPRESENTATION_SOURCE =
		"module Main\n"
		"\n"
		"def main = fn() -> Int => {\n"
		"    def a: Int = 3 as Int;\n"
		"    def b: Int = 4 as Int;\n"
		"    def sum: Int = (a as Int) + (b as Int);\n"
		"    def diff: Int = (a as Int) - (b as Int);\n"
		"    sum * diff\n"
		"};\n";
}

TEST_CASE("Newtype arithmetic compiles to byte-identical opcodes as its representation", "[backend][newtype]")
{
	const MarmotExecutable newtype_executable = CompileOrFail(NEWTYPE_SOURCE);
	const MarmotExecutable representation_executable = CompileOrFail(REPRESENTATION_SOURCE);

	const MarmotCompiler::Optional<int> newtype_main = FindMainProcedureIndex(newtype_executable);
	const MarmotCompiler::Optional<int> representation_main = FindMainProcedureIndex(representation_executable);

	REQUIRE(newtype_main.HasValue());
	REQUIRE(representation_main.HasValue());

	const int newtype_size = newtype_executable.GetByteCodeSize(newtype_main.Value());
	const int representation_size = representation_executable.GetByteCodeSize(representation_main.Value());

	REQUIRE(newtype_size == representation_size);
	REQUIRE(newtype_size > 0);

	for (int instr = 0; instr < newtype_size; instr += 1)
	{
		CAPTURE(instr);
		REQUIRE(newtype_executable.ReadByteCode(instr, newtype_main.Value()) == representation_executable.ReadByteCode(instr, representation_main.Value()));
	}
}

TEST_CASE("Newtype arithmetic never emits CONSTRUCT_UNION", "[backend][newtype]")
{
	const MarmotExecutable executable = CompileOrFail(NEWTYPE_SOURCE);
	const MarmotCompiler::Optional<int> main_index = FindMainProcedureIndex(executable);
	REQUIRE(main_index.HasValue());

	const int size = executable.GetByteCodeSize(main_index.Value());
	for (int instr = 0; instr < size; instr += 1)
	{
		REQUIRE(executable.ReadByteCode(instr, main_index.Value()) != OpCode::CONSTRUCT_UNION);
	}
}
