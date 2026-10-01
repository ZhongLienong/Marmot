#include <catch2/catch_test_macros.hpp>

#include "Bytecode/Executable/Executable.h"
#include "Bytecode/Disassembler/Disassembler.h"
#include "support/OutputCapture.h"

#include <string>
#include <vector>

// The disassembler decodes each instruction by hand. These tests hold its
// decoder to the VM's instruction table.

TEST_CASE("Every opcode has a length and a name", "[opcode]")
{
	for (size_t index = 0uz; index < VmOpCodeTable::COUNT; index += 1uz)
	{
		const VmOpCode opcode = static_cast<VmOpCode>(index);
		INFO(VmOpCodeTable::Name(opcode));
		CHECK(VmOpCodeTable::Length(opcode) >= 1);
		CHECK(VmOpCodeTable::Name(opcode) != "<invalid opcode>");
	}

	CHECK(VmOpCodeTable::Name(VmOpCode::LOAD_STRING_WIDE) == "LOAD_STRING_WIDE");
	CHECK(VmOpCodeTable::Name(VmOpCode::GET_LOCAL2) == "GET_LOCAL2");
	CHECK(VmOpCodeTable::Length(VmOpCode::INTEGER_CONSTANT) == 9);
	CHECK(VmOpCodeTable::Length(VmOpCode::IF_LOCAL_GE_LOCAL) == 5);
	CHECK(VmOpCodeTable::Length(VmOpCode::ADD_LOCAL_INT) == 3);
}

TEST_CASE("Byte values past the last opcode have no length", "[opcode]")
{
	for (size_t index = VmOpCodeTable::COUNT; index < 256uz; index += 1uz)
	{
		CHECK(VmOpCodeTable::Length(static_cast<VmOpCode>(index)) == 0);
	}
}

TEST_CASE("The disassembler reads each instruction's length from OpCodes.def", "[opcode][disassembler]")
{
	for (size_t index = 0uz; index < VmOpCodeTable::COUNT; index += 1uz)
	{
		const VmOpCode opcode = static_cast<VmOpCode>(index);
		const int length = VmOpCodeTable::Length(opcode);

		// The instruction with every operand byte zero, so each index it carries
		// names the first global, string or procedure.
		std::vector<VmOpCode> bytecode(static_cast<size_t>(length), static_cast<VmOpCode>(0));
		bytecode[0u] = opcode;
		VmBytecodeStream procedure(std::move(bytecode), std::vector<std::pair<int, int>>{ { 1, length } });

		VmExecutable executable;
		static_cast<void>(executable.AddGlobalVariable(std::string("global")));
		executable.AddStringPool(std::vector<std::string>{ "text" });
		executable.AttachProcedureNames(std::vector<std::string>{ "$main$" });
		executable.AttachProcedureSourcePaths(std::vector<std::string>{ "Main.mmt" });
		std::vector<VmBytecodeStream> procedures;
		procedures.emplace_back(std::move(procedure));
		executable.AttachProcedures(std::move(procedures));

		int offset = 0;
		MidoriTest::OutputCapture capture;
		Disassembler::DisassembleInstruction(stdout, executable, 0, offset);
		static_cast<void>(capture.Stop());

		INFO(VmOpCodeTable::Name(opcode));
		CHECK(offset == length);
	}
}
