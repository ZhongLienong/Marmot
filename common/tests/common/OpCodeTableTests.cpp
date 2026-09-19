#include <catch2/catch_test_macros.hpp>

#include "Common/Executable/Executable.h"
#include "Utility/Disassembler/Disassembler.h"
#include "support/OutputCapture.h"

#include <string>
#include <vector>

// OpCodes.def is the one list of instructions and their lengths. The linker and
// the code generator step through bytecode with OpCodeTable::Length; the
// disassembler still decodes each instruction by hand. These tests hold the
// hand-written decoder to the table.

TEST_CASE("Every opcode has a length and a name", "[opcode]")
{
	for (size_t index = 0uz; index < OpCodeTable::COUNT; index += 1uz)
	{
		const OpCode opcode = static_cast<OpCode>(index);
		INFO(OpCodeTable::Name(opcode));
		CHECK(OpCodeTable::Length(opcode) >= 1);
		CHECK(OpCodeTable::Name(opcode) != "<invalid opcode>");
	}

	CHECK(OpCodeTable::Name(OpCode::LOAD_STRING) == "LOAD_STRING");
	CHECK(OpCodeTable::Name(OpCode::GET_LOCAL2) == "GET_LOCAL2");
	CHECK(OpCodeTable::Length(OpCode::INTEGER_CONSTANT) == 9);
	CHECK(OpCodeTable::Length(OpCode::IF_LOCAL_GE_LOCAL) == 7);
}

TEST_CASE("Byte values past the last opcode have no length", "[opcode]")
{
	for (size_t index = OpCodeTable::COUNT; index < 256uz; index += 1uz)
	{
		CHECK(OpCodeTable::Length(static_cast<OpCode>(index)) == 0);
	}
}

#if MIDORI_ENABLE_DISASSEMBLY
TEST_CASE("The disassembler reads each instruction's length from OpCodes.def", "[opcode][disassembler]")
{
	for (size_t index = 0uz; index < OpCodeTable::COUNT; index += 1uz)
	{
		const OpCode opcode = static_cast<OpCode>(index);
		const int length = OpCodeTable::Length(opcode);

		// The instruction with every operand byte zero, so each index it carries
		// names the first global, string or procedure.
		BytecodeStream procedure;
		procedure.AddByteCode(opcode, 1);
		for (int operand = 1; operand < length; operand += 1)
		{
			procedure.AddByteCode(static_cast<OpCode>(0), 1);
		}

		MidoriExecutable executable;
		static_cast<void>(executable.AddGlobalVariable(std::string("global")));
		executable.AddStringPool(std::vector<std::string>{ "text" });
		executable.AttachProcedureNames(std::vector<std::string>{ "$main$" });
		executable.AttachProcedureSourcePaths(std::vector<std::string>{ "Main.mmt" });
		std::vector<BytecodeStream> procedures;
		procedures.emplace_back(std::move(procedure));
		executable.AttachProcedures(std::move(procedures));

		int offset = 0;
		MidoriTest::OutputCapture capture;
		Disassembler::DisassembleInstruction(executable, 0, offset);
		static_cast<void>(capture.Stop());

		INFO(OpCodeTable::Name(opcode));
		CHECK(offset == length);
	}
}
#endif
