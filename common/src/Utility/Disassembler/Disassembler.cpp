#include <iomanip>
#include <sstream>
#include <string>

#include "Common/Executable/Executable.h"
#include "Common/Printer/Printer.h"
#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"
#include "Disassembler.h"

#if MIDORI_ENABLE_DISASSEMBLY

namespace
{
	constexpr int address_width = 6;
	constexpr int instr_width = 25;
	constexpr int operand_width = 12;
	constexpr int comment_width = 30;

	void SimpleInstruction(std::string_view name, int& offset)
	{
		offset += 1;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void PopMultipleInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void NumericConstantInstruction(bool is_integer, std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		std::byte operand_bytes[8];
		for (int i = 0; i < 8; i += 1)
		{
			operand_bytes[i] = static_cast<std::byte>(executable.ReadByteCode(offset + 1 + i, proc_index));
		}
		offset += 9;

		std::ostringstream formated_str;
		MidoriFloat as_float = *reinterpret_cast<MidoriFloat*>(operand_bytes);
		MidoriInteger as_integer = *reinterpret_cast<MidoriInteger*>(operand_bytes);

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		if (is_integer)
		{
			formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(as_integer));
			formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// " + std::to_string(as_integer));
		}
		else
		{
			formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(as_float));
			formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// " + std::to_string(as_float));
		}
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void ByteConstantInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		MidoriByte operand = static_cast<MidoriByte>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;

		std::ostringstream formated_str;
		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(static_cast<unsigned int>(operand)));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// 0x" + (std::ostringstream() << std::hex << std::uppercase << static_cast<unsigned int>(operand)).str());
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void WordConstantInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		std::byte operand_bytes[8];
		for (int i = 0; i < 8; i += 1)
		{
			operand_bytes[i] = static_cast<std::byte>(executable.ReadByteCode(offset + 1 + i, proc_index));
		}
		offset += 9;

		std::ostringstream formated_str;
		MidoriWord operand = *reinterpret_cast<MidoriWord*>(operand_bytes);

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// 0x" + (std::ostringstream() << std::hex << std::uppercase << operand).str());
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void LoadStringInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(index));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// string pool index");
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void LoadStringWideInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int index = low_byte | (high_byte << 8);
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(index));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// string pool index");
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void JumpInstruction(std::string_view name, int sign, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) |
			(static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8);
		offset += 3;
		std::ostringstream formated_str;

		int destination = offset + sign * operand;
		std::ostringstream dest_str;
		dest_str << "-> 0x" << std::hex << std::setfill('0') << std::setw(address_width) << destination;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_YELLOW>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>(dest_str.str());
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void GlobalVariableInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// " + executable.GetGlobalVariable(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void LocalOrCellVariableInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// offset " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void GlobalVariableWideInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int operand = (high_byte << 8) | low_byte;
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// " + executable.GetGlobalVariable(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void LocalOrCellVariableWideInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int operand = (high_byte << 8) | low_byte;
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// offset " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void AggregateCreateInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) |
			(static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8) |
			(static_cast<int>(executable.ReadByteCode(offset + 3, proc_index)) << 16);
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// element count: " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void ClosureCreateInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int captured_count = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(captured_count));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// number of captured variables: " + std::to_string(captured_count));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void AllocateClosureInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(index));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// code index: " + std::to_string(index));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// number of parameters: " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallFixedInstruction(std::string_view name, int arity, int& offset)
	{
		offset += 1;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// number of parameters: " + std::to_string(arity));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallDirectInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int target_proc = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int arity = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(target_proc));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// proc: " + std::to_string(target_proc) + ", params: " + std::to_string(arity));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallDirectFixedInstruction(std::string_view name, int arity, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int target_proc = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(target_proc));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// proc: " + std::to_string(target_proc) + ", params: " + std::to_string(arity));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallGlobalInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int global_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int arity = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(global_index));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// global: " + executable.GetGlobalVariable(global_index) + ", params: " + std::to_string(arity));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallGlobalWideInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int global_index = (high_byte << 8) | low_byte;
		int arity = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(global_index));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// global: " + executable.GetGlobalVariable(global_index) + ", params: " + std::to_string(arity));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallForeignInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int arity = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int return_type = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(return_type));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// params: " + std::to_string(arity) + ", return: ");

		std::string return_type_str = (return_type == 0) ? "primitive" : (return_type == 1) ? "text" : (return_type == 2) ? "array" : "unknown";
		formated_str << return_type_str;
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void CallForeignIndexedInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int ffi_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int arity = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int return_type = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(ffi_index));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arity));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(return_type));

		std::string ffi_name = (static_cast<size_t>(ffi_index) < MidoriFFIRegistry::BUILTIN_COUNT)
			? MidoriFFIRegistry::GetEntry(static_cast<size_t>(ffi_index)).m_name
			: "unknown";
		std::string return_type_str = (return_type == 0) ? "primitive" : (return_type == 1) ? "text" : (return_type == 2) ? "array" : "unknown";

		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// " + ffi_name + ", params: " + std::to_string(arity) + ", return: " + return_type_str);
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void MemberInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// member index: " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void DataInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// data size: " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void JoinWorkerInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		const int ok_tag = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		const int err_tag = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		const int cancelled_tag = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		const int failed_tag = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
		offset += 5;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::format("{} {} {} {}", ok_tag, err_tag, cancelled_tag, failed_tag));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>(std::format("// tags: Ok {}, Err {}, Cancelled {}, Failed {}", ok_tag, err_tag, cancelled_tag, failed_tag));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void SetTagInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// union tag: " + std::to_string(operand));
		formated_str << '\n';
		Printer::Print(formated_str.str());
	}

	void SpawnWorkerInstruction(std::string_view name, const MidoriExecutable& executable, int proc_index, int& offset)
	{
		const int arg_count = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));

		std::ostringstream formatted_str;
		formatted_str << Printer::Colored<Printer::Color::BRIGHT_WHITE>(std::string(name));
		formatted_str << " " << Printer::Colored<Printer::Color::CYAN>(std::to_string(arg_count));
		formatted_str << "  " << Printer::Colored<Printer::Color::DARK_GRAY>("// argc " + std::to_string(arg_count) + ", function on the stack");
		formatted_str << '\n';
		Printer::Print(formatted_str.str());

		offset += 2;
	}
}

namespace Disassembler
{
	// Forward declaration
	void DisassembleInstruction(const MidoriExecutable& executable, int proc_index, int& offset);

	void DisassembleBytecodeStream(const MidoriExecutable& executable, int proc_index, std::string_view proc_name)
	{
		std::ostringstream header;
		header << std::string(95, '=') << "\n";
		header << " " << Printer::Colored<Printer::Color::BRIGHT_CYAN>(std::string(proc_name)) << "\n";
		header << std::string(95, '=') << "\n";
		Printer::Print(header.str());

		int offset = 0;
		while (offset < executable.GetByteCodeSize(proc_index))
		{
			DisassembleInstruction(executable, proc_index, offset);
		}

		std::ostringstream footer;
		footer << Printer::Colored<Printer::Color::DARK_GRAY>(std::string(95, '-')) << "\n\n";
		Printer::Print(footer.str());
	}

	void DisassembleInstruction(const MidoriExecutable& executable, int proc_index, int& offset)
	{
		std::ostringstream formated_str;
		formated_str << '[' << std::right << std::setfill('0') << std::setw(::address_width) << std::hex << offset << "] " << std::setfill(' ');

		formated_str << std::setw(::address_width) << std::left;
		if (offset > 0 && executable.GetLine(offset, proc_index) == executable.GetLine(offset - 1, proc_index))
		{
			formated_str << "|" << std::setfill(' ');
		}
		else
		{
			formated_str << std::dec << executable.GetLine(offset, proc_index) << std::setfill(' ');
		}
		formated_str << std::right << ' ';
		Printer::Print(formated_str.str());

		OpCode instruction = executable.ReadByteCode(offset, proc_index);
		switch (instruction) 
		{
		case OpCode::LOAD_STRING:
			LoadStringInstruction("LOAD_STRING", executable, proc_index, offset);
			break;
		case OpCode::LOAD_STRING_WIDE:
			LoadStringWideInstruction("LOAD_STRING_WIDE", executable, proc_index, offset);
			break;
		case OpCode::INTEGER_CONSTANT:
			NumericConstantInstruction(true, "INTEGER_CONSTANT", executable, proc_index, offset);
			break;
		case OpCode::FLOAT_CONSTANT:
			NumericConstantInstruction(false, "FLOAT_CONSTANT", executable, proc_index, offset);
			break;
		case OpCode::BYTE_CONSTANT:
			ByteConstantInstruction("BYTE_CONSTANT", executable, proc_index, offset);
			break;
		case OpCode::WORD_CONSTANT:
			WordConstantInstruction("WORD_CONSTANT", executable, proc_index, offset);
			break;
		case OpCode::OP_UNIT:
			SimpleInstruction("OP_UNIT", offset);
			break;
		case OpCode::OP_TRUE:
			SimpleInstruction("OP_TRUE", offset);
			break;
		case OpCode::OP_FALSE:
			SimpleInstruction("OP_FALSE", offset);
			break;
		case OpCode::INT_MINUS_1:
			SimpleInstruction("INT_MINUS_1", offset);
			break;
		case OpCode::INT_0:
			SimpleInstruction("INT_0", offset);
			break;
		case OpCode::INT_1:
			SimpleInstruction("INT_1", offset);
			break;
		case OpCode::INT_2:
			SimpleInstruction("INT_2", offset);
			break;
		case OpCode::INT_3:
			SimpleInstruction("INT_3", offset);
			break;
		case OpCode::INT_4:
			SimpleInstruction("INT_4", offset);
			break;
		case OpCode::INT_5:
			SimpleInstruction("INT_5", offset);
			break;
		case OpCode::INT_10:
			SimpleInstruction("INT_10", offset);
			break;
		case OpCode::CREATE_ARRAY:
			AggregateCreateInstruction("CREATE_ARRAY", executable, proc_index, offset);
			break;
		case OpCode::CREATE_TUPLE:
			AggregateCreateInstruction("CREATE_TUPLE", executable, proc_index, offset);
			break;
		case OpCode::GET_ARRAY:
			SimpleInstruction("GET_ARRAY", offset);
			break;
		case OpCode::GET_TUPLE:
			SimpleInstruction("GET_TUPLE", offset);
			break;
		case OpCode::UNPACK_TUPLE:
			SimpleInstruction("UNPACK_TUPLE", offset);
			break;
		case OpCode::ADD_BACK_ARRAY:
			SimpleInstruction("ADD_BACK_ARRAY", offset);
			break;
		case OpCode::GET_ARRAY_LENGTH:
			SimpleInstruction("GET_ARRAY_LENGTH", offset);
			break;
		case OpCode::CREATE_INT_RANGE:
			SimpleInstruction("CREATE_INT_RANGE", offset);
			break;
		case OpCode::CREATE_FLOAT_RANGE:
			SimpleInstruction("CREATE_FLOAT_RANGE", offset);
			break;
		case OpCode::GET_RANGE_START:
			SimpleInstruction("GET_RANGE_START", offset);
			break;
		case OpCode::GET_RANGE_END:
			SimpleInstruction("GET_RANGE_END", offset);
			break;
		case OpCode::GET_RANGE_STEP:
			SimpleInstruction("GET_RANGE_STEP", offset);
			break;
		case OpCode::INT_TO_FLOAT:
			SimpleInstruction("INT_TO_FLOAT", offset);
			break;
		case OpCode::TEXT_TO_FLOAT:
			SimpleInstruction("TEXT_TO_FLOAT", offset);
			break;
		case OpCode::FLOAT_TO_INT:
			SimpleInstruction("FLOAT_TO_INT", offset);
			break;
		case OpCode::TEXT_TO_INT:
			SimpleInstruction("TEXT_TO_INT", offset);
			break;
		case OpCode::FLOAT_TO_TEXT:
			SimpleInstruction("FLOAT_TO_TEXT", offset);
			break;
		case OpCode::INT_TO_TEXT:
			SimpleInstruction("INT_TO_TEXT", offset);
			break;
		case OpCode::WORD_TO_TEXT:
			SimpleInstruction("WORD_TO_TEXT", offset);
			break;
		case OpCode::BYTE_TO_INT:
			SimpleInstruction("BYTE_TO_INT", offset);
			break;
		case OpCode::INT_TO_BYTE:
			SimpleInstruction("INT_TO_BYTE", offset);
			break;
		case OpCode::BYTE_TO_WORD:
			SimpleInstruction("BYTE_TO_WORD", offset);
			break;
		case OpCode::WORD_TO_BYTE:
			SimpleInstruction("WORD_TO_BYTE", offset);
			break;
		case OpCode::WORD_TO_INT:
			SimpleInstruction("WORD_TO_INT", offset);
			break;
		case OpCode::INT_TO_WORD:
			SimpleInstruction("INT_TO_WORD", offset);
			break;
		case OpCode::BYTE_TO_FLOAT:
			SimpleInstruction("BYTE_TO_FLOAT", offset);
			break;
		case OpCode::FLOAT_TO_BYTE:
			SimpleInstruction("FLOAT_TO_BYTE", offset);
			break;
		case OpCode::WORD_TO_FLOAT:
			SimpleInstruction("WORD_TO_FLOAT", offset);
			break;
		case OpCode::FLOAT_TO_WORD:
			SimpleInstruction("FLOAT_TO_WORD", offset);
			break;
		case OpCode::LEFT_SHIFT:
			SimpleInstruction("LEFT_SHIFT", offset);
			break;
		case OpCode::RIGHT_SHIFT:
			SimpleInstruction("RIGHT_SHIFT", offset);
			break;
		case OpCode::LEFT_SHIFT_BYTE:
			SimpleInstruction("LEFT_SHIFT_BYTE", offset);
			break;
		case OpCode::RIGHT_SHIFT_BYTE:
			SimpleInstruction("RIGHT_SHIFT_BYTE", offset);
			break;
		case OpCode::LEFT_SHIFT_WORD:
			SimpleInstruction("LEFT_SHIFT_WORD", offset);
			break;
		case OpCode::RIGHT_SHIFT_WORD:
			SimpleInstruction("RIGHT_SHIFT_WORD", offset);
			break;
		case OpCode::BITWISE_AND:
			SimpleInstruction("BITWISE_AND", offset);
			break;
		case OpCode::BITWISE_OR:
			SimpleInstruction("BITWISE_OR", offset);
			break;
		case OpCode::BITWISE_XOR:
			SimpleInstruction("BITWISE_XOR", offset);
			break;
		case OpCode::BITWISE_NOT:
			SimpleInstruction("BITWISE_NOT", offset);
			break;
		case OpCode::ADD_FLOAT:
			SimpleInstruction("ADD_FLOAT", offset);
			break;
		case OpCode::SUBTRACT_FLOAT:
			SimpleInstruction("SUBTRACT_FLOAT", offset);
			break;
		case OpCode::MULTIPLY_FLOAT:
			SimpleInstruction("MULTIPLY_FLOAT", offset);
			break;
		case OpCode::DIVIDE_FLOAT:
			SimpleInstruction("DIVIDE_FLOAT", offset);
			break;
		case OpCode::MODULO_FLOAT:
			SimpleInstruction("MODULO_FLOAT", offset);
			break;
		case OpCode::ADD_INTEGER:
			SimpleInstruction("ADD_INTEGER", offset);
			break;
		case OpCode::SUBTRACT_INTEGER:
			SimpleInstruction("SUBTRACT_INTEGER", offset);
			break;
		case OpCode::MULTIPLY_INTEGER:
			SimpleInstruction("MULTIPLY_INTEGER", offset);
			break;
		case OpCode::DIVIDE_INTEGER:
			SimpleInstruction("DIVIDE_INTEGER", offset);
			break;
		case OpCode::MODULO_INTEGER:
			SimpleInstruction("MODULO_INTEGER", offset);
			break;
		case OpCode::ADD_BYTE:
			SimpleInstruction("ADD_BYTE", offset);
			break;
		case OpCode::SUBTRACT_BYTE:
			SimpleInstruction("SUBTRACT_BYTE", offset);
			break;
		case OpCode::MULTIPLY_BYTE:
			SimpleInstruction("MULTIPLY_BYTE", offset);
			break;
		case OpCode::DIVIDE_BYTE:
			SimpleInstruction("DIVIDE_BYTE", offset);
			break;
		case OpCode::MODULO_BYTE:
			SimpleInstruction("MODULO_BYTE", offset);
			break;
		case OpCode::ADD_WORD:
			SimpleInstruction("ADD_WORD", offset);
			break;
		case OpCode::SUBTRACT_WORD:
			SimpleInstruction("SUBTRACT_WORD", offset);
			break;
		case OpCode::MULTIPLY_WORD:
			SimpleInstruction("MULTIPLY_WORD", offset);
			break;
		case OpCode::DIVIDE_WORD:
			SimpleInstruction("DIVIDE_WORD", offset);
			break;
		case OpCode::MODULO_WORD:
			SimpleInstruction("MODULO_WORD", offset);
			break;
		case OpCode::CONCAT_ARRAY:
			SimpleInstruction("CONCAT_ARRAY", offset);
			break;
		case OpCode::CONCAT_TEXT:
			SimpleInstruction("CONCAT_TEXT", offset);
			break;
		case OpCode::ADD_ASSIGN_INT:
			SimpleInstruction("ADD_ASSIGN_INT", offset);
			break;
		case OpCode::SUB_ASSIGN_INT:
			SimpleInstruction("SUB_ASSIGN_INT", offset);
			break;
		case OpCode::EQUAL_FLOAT:
			SimpleInstruction("EQUAL_FLOAT", offset);
			break;
		case OpCode::NOT_EQUAL_FLOAT:
			SimpleInstruction("NOT_EQUAL_FLOAT", offset);
			break;
		case OpCode::GREATER_FLOAT:
			SimpleInstruction("GREATER_FLOAT", offset);
			break;
		case OpCode::GREATER_EQUAL_FLOAT:
			SimpleInstruction("GREATER_EQUAL_FLOAT", offset);
			break;
		case OpCode::LESS_FLOAT:
			SimpleInstruction("LESS_FLOAT", offset);
			break;
		case OpCode::LESS_EQUAL_FLOAT:
			SimpleInstruction("LESS_EQUAL_FLOAT", offset);
			break;
		case OpCode::EQUAL_INTEGER:
			SimpleInstruction("EQUAL_INTEGER", offset);
			break;
		case OpCode::NOT_EQUAL_INTEGER:
			SimpleInstruction("NOT_EQUAL_INTEGER", offset);
			break;
		case OpCode::GREATER_INTEGER:
			SimpleInstruction("GREATER_INTEGER", offset);
			break;
		case OpCode::GREATER_EQUAL_INTEGER:
			SimpleInstruction("GREATER_EQUAL_INTEGER", offset);
			break;
		case OpCode::LESS_INTEGER:
			SimpleInstruction("LESS_INTEGER", offset);
			break;
		case OpCode::LESS_EQUAL_INTEGER:
			SimpleInstruction("LESS_EQUAL_INTEGER", offset);
			break;
		case OpCode::EQUAL_BYTE:
			SimpleInstruction("EQUAL_BYTE", offset);
			break;
		case OpCode::NOT_EQUAL_BYTE:
			SimpleInstruction("NOT_EQUAL_BYTE", offset);
			break;
		case OpCode::GREATER_BYTE:
			SimpleInstruction("GREATER_BYTE", offset);
			break;
		case OpCode::GREATER_EQUAL_BYTE:
			SimpleInstruction("GREATER_EQUAL_BYTE", offset);
			break;
		case OpCode::LESS_BYTE:
			SimpleInstruction("LESS_BYTE", offset);
			break;
		case OpCode::LESS_EQUAL_BYTE:
			SimpleInstruction("LESS_EQUAL_BYTE", offset);
			break;
		case OpCode::EQUAL_WORD:
			SimpleInstruction("EQUAL_WORD", offset);
			break;
		case OpCode::NOT_EQUAL_WORD:
			SimpleInstruction("NOT_EQUAL_WORD", offset);
			break;
		case OpCode::GREATER_WORD:
			SimpleInstruction("GREATER_WORD", offset);
			break;
		case OpCode::GREATER_EQUAL_WORD:
			SimpleInstruction("GREATER_EQUAL_WORD", offset);
			break;
		case OpCode::LESS_WORD:
			SimpleInstruction("LESS_WORD", offset);
			break;
		case OpCode::LESS_EQUAL_WORD:
			SimpleInstruction("LESS_EQUAL_WORD", offset);
			break;
		case OpCode::EQUAL_TEXT:
			SimpleInstruction("EQUAL_TEXT", offset);
			break;
		case OpCode::NOT:
			SimpleInstruction("NOT", offset);
			break;
		case OpCode::NEGATE_FLOAT:
			SimpleInstruction("NEGATE_FLOAT", offset);
			break;
		case OpCode::NEGATE_INTEGER:
			SimpleInstruction("NEGATE_INTEGER", offset);
			break;
		case OpCode::JUMP_IF_FALSE:
			JumpInstruction("JUMP_IF_FALSE", 1, executable, proc_index, offset);
			break;
		case OpCode::JUMP_IF_TRUE:
			JumpInstruction("JUMP_IF_TRUE", 1, executable, proc_index, offset);
			break;
		case OpCode::JUMP:
			JumpInstruction("JUMP", 1, executable, proc_index, offset);
			break;
		case OpCode::JUMP_BACK:
			JumpInstruction("JUMP_BACK", -1, executable, proc_index, offset);
			break;
		case OpCode::IF_INTEGER_LESS:
			JumpInstruction("IF_INTEGER_LESS", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_INTEGER_LESS_EQUAL:
			JumpInstruction("IF_INTEGER_LESS_EQUAL", 1, executable, proc_index, offset);
				break;
		case OpCode::IF_INTEGER_GREATER:
			JumpInstruction("IF_INTEGER_GREATER", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_INTEGER_GREATER_EQUAL:
			JumpInstruction("IF_INTEGER_GREATER_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_INTEGER_EQUAL:
			JumpInstruction("IF_INTEGER_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_INTEGER_NOT_EQUAL:
			JumpInstruction("IF_INTEGER_NOT_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_LESS:
			JumpInstruction("IF_FLOAT_LESS", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_LESS_EQUAL:
			JumpInstruction("IF_FLOAT_LESS_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_GREATER:
			JumpInstruction("IF_FLOAT_GREATER", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_GREATER_EQUAL:
			JumpInstruction("IF_FLOAT_GREATER_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_EQUAL:
			JumpInstruction("IF_FLOAT_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::IF_FLOAT_NOT_EQUAL:
			JumpInstruction("IF_FLOAT_NOT_EQUAL", 1, executable, proc_index, offset);
			break;
		case OpCode::LOAD_TAG:
			SimpleInstruction("LOAD_TAG", offset);
			break;
		case OpCode::GET_TAG:
			SimpleInstruction("GET_TAG", offset);
			break;
		case OpCode::SET_TAG:
			SetTagInstruction("SET_TAG", executable, proc_index, offset);
			break;
		case OpCode::SPAWN_WORKER:
			SpawnWorkerInstruction("SPAWN_WORKER", executable, proc_index, offset);
			break;
		case OpCode::JOIN_WORKER:
			JoinWorkerInstruction("JOIN_WORKER", executable, proc_index, offset);
			break;
		case OpCode::CHANNEL_CREATE:
			SimpleInstruction("CHANNEL_CREATE", offset);
			break;
		case OpCode::CHANNEL_SEND:
			SimpleInstruction("CHANNEL_SEND", offset);
			break;
		case OpCode::CHANNEL_RECEIVE:
			SimpleInstruction("CHANNEL_RECEIVE", offset);
			break;
		case OpCode::CHANNEL_CLOSE:
			SimpleInstruction("CHANNEL_CLOSE", offset);
			break;
		case OpCode::WORKER_IS_DONE:
			SimpleInstruction("WORKER_IS_DONE", offset);
			break;
		case OpCode::MAKE_CELL:
			SimpleInstruction("MAKE_CELL", offset);
			break;
		case OpCode::READ_CELL:
			SimpleInstruction("READ_CELL", offset);
			break;
		case OpCode::WRITE_CELL:
			SimpleInstruction("WRITE_CELL", offset);
			break;
		case OpCode::WORKER_CANCEL:
			SimpleInstruction("WORKER_CANCEL", offset);
			break;
		case OpCode::ADD_LOCAL_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			Printer::Print(std::string(Printer::Colored<Printer::Color::BRIGHT_WHITE>("ADD_LOCAL_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + "\n");
			offset += 6;
			break;
		}
		case OpCode::PUSH_LOCAL_SUB_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			Printer::Print(std::string(Printer::Colored<Printer::Color::BRIGHT_WHITE>("PUSH_LOCAL_SUB_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + "\n");
			offset += 4;
			break;
		}
		case OpCode::IF_LOCAL_LE_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 5, proc_index));
			Printer::Print(std::string(Printer::Colored<Printer::Color::BRIGHT_WHITE>("IF_LOCAL_LE_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + " -> " + std::to_string(offset + 6 + (low | (high << 8))) + "\n");
			offset += 6;
			break;
		}
		case OpCode::IF_LOCAL_GE_LOCAL:
		{
			const int left = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int right = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 5, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 6, proc_index));
			Printer::Print(std::string(Printer::Colored<Printer::Color::BRIGHT_WHITE>("IF_LOCAL_GE_LOCAL")) + " left=" + std::to_string(left) + " right=" + std::to_string(right) + " -> " + std::to_string(offset + 7 + (low | (high << 8))) + "\n");
			offset += 7;
			break;
		}
		case OpCode::GET_LOCAL2:
		{
			const int first = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int second = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			Printer::Print(std::string(Printer::Colored<Printer::Color::BRIGHT_WHITE>("GET_LOCAL2")) + " first=" + std::to_string(first) + " second=" + std::to_string(second) + "\n");
			offset += 4;
			break;
		}
		case OpCode::EXTEND_ARRAY:
			SimpleInstruction("EXTEND_ARRAY", offset);
			break;
		case OpCode::EXTEND_TEXT:
			SimpleInstruction("EXTEND_TEXT", offset);
			break;
		case OpCode::CALL_FOREIGN:
			CallForeignInstruction("CALL_FOREIGN", executable, proc_index, offset);
			break;
		case OpCode::CALL_FOREIGN_INDEXED:
			CallForeignIndexedInstruction("CALL_FOREIGN_INDEXED", executable, proc_index, offset);
			break;
		case OpCode::CALL:
			CallInstruction("CALL", executable, proc_index, offset);
			break;
		case OpCode::CALL_0:
			CallFixedInstruction("CALL_0", 0, offset);
			break;
		case OpCode::CALL_1:
			CallFixedInstruction("CALL_1", 1, offset);
			break;
		case OpCode::CALL_2:
			CallFixedInstruction("CALL_2", 2, offset);
			break;
		case OpCode::CALL_3:
			CallFixedInstruction("CALL_3", 3, offset);
			break;
		case OpCode::CALL_PROC:
			CallDirectInstruction("CALL_PROC", executable, proc_index, offset);
			break;
		case OpCode::CALL_PROC_0:
			CallDirectFixedInstruction("CALL_PROC_0", 0, executable, proc_index, offset);
			break;
		case OpCode::CALL_PROC_1:
			CallDirectFixedInstruction("CALL_PROC_1", 1, executable, proc_index, offset);
			break;
		case OpCode::CALL_PROC_2:
			CallDirectFixedInstruction("CALL_PROC_2", 2, executable, proc_index, offset);
			break;
		case OpCode::CALL_PROC_3:
			CallDirectFixedInstruction("CALL_PROC_3", 3, executable, proc_index, offset);
			break;
		case OpCode::CALL_GLOBAL:
			CallGlobalInstruction("CALL_GLOBAL", executable, proc_index, offset);
			break;
		case OpCode::TAIL_CALL:
			CallInstruction("TAIL_CALL", executable, proc_index, offset);
			break;
		case OpCode::CONSTRUCT_STRUCT:
			DataInstruction("CONSTRUCT_STRUCT", executable, proc_index, offset);
			break;
		case OpCode::CONSTRUCT_UNION:
			DataInstruction("CONSTRUCT_UNION", executable, proc_index, offset);
			break;
		case OpCode::LOAD_EMPTY_UNION:
			DataInstruction("LOAD_EMPTY_UNION", executable, proc_index, offset);
			break;
		case OpCode::MAKE_CLOSURE:
			AllocateClosureInstruction("MAKE_CLOSURE", executable, proc_index, offset);
			break;
		case OpCode::BIND_CAPTURES:
			ClosureCreateInstruction("BIND_CAPTURES", executable, proc_index, offset);
			break;
		case OpCode::MAKE_FUNCTION:
			AllocateClosureInstruction("MAKE_FUNCTION", executable, proc_index, offset);
			break;
		case OpCode::DEFINE_GLOBAL:
			GlobalVariableInstruction("DEFINE_GLOBAL", executable, proc_index, offset);
			break;
		case OpCode::GET_GLOBAL:
			GlobalVariableInstruction("GET_GLOBAL", executable, proc_index, offset);
			break;
		case OpCode::SET_GLOBAL:
			GlobalVariableInstruction("SET_GLOBAL", executable, proc_index, offset);
			break;
		case OpCode::GET_LOCAL:
			LocalOrCellVariableInstruction("GET_LOCAL", executable, proc_index, offset);
			break;
		case OpCode::SET_LOCAL:
			LocalOrCellVariableInstruction("SET_LOCAL", executable, proc_index, offset);
			break;
		case OpCode::GET_LOCAL_CELL:
			LocalOrCellVariableInstruction("GET_LOCAL_CELL", executable, proc_index, offset);
			break;
		case OpCode::SET_LOCAL_CELL:
			LocalOrCellVariableInstruction("SET_LOCAL_CELL", executable, proc_index, offset);
			break;
		case OpCode::GET_CELL:
			LocalOrCellVariableInstruction("GET_CELL", executable, proc_index, offset);
			break;
		case OpCode::SET_CELL:
			LocalOrCellVariableInstruction("SET_CELL", executable, proc_index, offset);
			break;
		case OpCode::DEFINE_GLOBAL_WIDE:
			GlobalVariableWideInstruction("DEFINE_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::GET_GLOBAL_WIDE:
			GlobalVariableWideInstruction("GET_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::SET_GLOBAL_WIDE:
			GlobalVariableWideInstruction("SET_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::CALL_GLOBAL_WIDE:
			CallGlobalWideInstruction("CALL_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::GET_LOCAL_WIDE:
			LocalOrCellVariableWideInstruction("GET_LOCAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::SET_LOCAL_WIDE:
			LocalOrCellVariableWideInstruction("SET_LOCAL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::GET_LOCAL_CELL_WIDE:
			LocalOrCellVariableWideInstruction("GET_LOCAL_CELL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::SET_LOCAL_CELL_WIDE:
			LocalOrCellVariableWideInstruction("SET_LOCAL_CELL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::GET_CELL_WIDE:
			LocalOrCellVariableWideInstruction("GET_CELL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::SET_CELL_WIDE:
			LocalOrCellVariableWideInstruction("SET_CELL_WIDE", executable, proc_index, offset);
			break;
		case OpCode::GET_MEMBER:
			MemberInstruction("GET_MEMBER", executable, proc_index, offset);
			break;
		case OpCode::POP:
			SimpleInstruction("POP", offset);
			break;
		case OpCode::DUP:
			SimpleInstruction("DUP", offset);
			break;
		case OpCode::SWAP:
			SimpleInstruction("SWAP", offset);
			break;
		case OpCode::POP_LOCAL_SCOPE:
			PopMultipleInstruction("POP_LOCAL_SCOPE", executable, proc_index, offset);
			break;
		case OpCode::POP_VALUES:
			PopMultipleInstruction("POP_VALUES", executable, proc_index, offset);
			break;
		case OpCode::POP_BLOCK_SCOPE:
			PopMultipleInstruction("POP_BLOCK_SCOPE", executable, proc_index, offset);
			break;
		case OpCode::POP_MATCH_SCOPE:
			PopMultipleInstruction("POP_MATCH_SCOPE", executable, proc_index, offset);
			break;
		case OpCode::RETURN:
			SimpleInstruction("RETURN", offset);
			break;
		case OpCode::HALT:
			SimpleInstruction("HALT", offset);
			break;
		case OpCode::PUSH_PLACEHOLDER:
			SimpleInstruction("PUSH_PLACEHOLDER", offset);
			break;
		default:
#ifdef _MSC_VER
			__assume(0);
#else
			__builtin_unreachable();
#endif
		}
	}
}
#endif
