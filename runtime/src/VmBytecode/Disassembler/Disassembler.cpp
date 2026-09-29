#include <format>
#include <iomanip>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "VmBytecode/Executable/Executable.h"
#include "VmBytecode/Scalar/Scalar.h"
#include "VmBytecode/Builtins/BuiltinTable.h"
#include "Disassembler.h"

#if MIDORI_ENABLE_DISASSEMBLY

namespace
{
	namespace Terminal
	{
		enum class Color
		{
			CYAN,
			WHITE,
			BRIGHT_YELLOW,
			BRIGHT_CYAN,
			BRIGHT_WHITE,
			DARK_GRAY,
		};

		constexpr std::string_view RESET = "[0m";

		constexpr std::string_view Code(Color color)
		{
			switch (color)
			{
			case Color::CYAN:
				return "[36m";
			case Color::WHITE:
				return "[37m";
			case Color::BRIGHT_YELLOW:
				return "[93m";
			case Color::BRIGHT_CYAN:
				return "[96m";
			case Color::BRIGHT_WHITE:
				return "[97m";
			case Color::DARK_GRAY:
				return "[38;5;240m";
			}
			std::unreachable();
		}

		template<Color color>
		std::string Colored(std::string_view text)
		{
			return std::format("{}{}{}", Code(color), text, RESET);
		}

		void Print(std::string_view message)
		{
			std::print("{}{}{}", Code(Color::WHITE), message, RESET);
		}
	}

	constexpr int address_width = 6;
	constexpr int instr_width = 25;
	constexpr int operand_width = 12;
	constexpr int comment_width = 30;

	void SimpleInstruction(std::string_view name, int& offset)
	{
		offset += 1;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void NumericConstantInstruction(bool is_integer, std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
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

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		if (is_integer)
		{
			formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(as_integer));
			formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// " + std::to_string(as_integer));
		}
		else
		{
			formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(as_float));
			formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// " + std::to_string(as_float));
		}
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void ByteConstantInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		MidoriByte operand = static_cast<MidoriByte>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;

		std::ostringstream formated_str;
		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(static_cast<unsigned int>(operand)));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// 0x" + (std::ostringstream() << std::hex << std::uppercase << static_cast<unsigned int>(operand)).str());
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void WordConstantInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		std::byte operand_bytes[8];
		for (int i = 0; i < 8; i += 1)
		{
			operand_bytes[i] = static_cast<std::byte>(executable.ReadByteCode(offset + 1 + i, proc_index));
		}
		offset += 9;

		std::ostringstream formated_str;
		MidoriWord operand = *reinterpret_cast<MidoriWord*>(operand_bytes);

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// 0x" + (std::ostringstream() << std::hex << std::uppercase << operand).str());
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void LoadStringWideInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int index = low_byte | (high_byte << 8);
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(index));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// string pool index");
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void JumpInstruction(std::string_view name, int sign, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) |
			(static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8);
		offset += 3;
		std::ostringstream formated_str;

		int destination = offset + sign * operand;
		std::ostringstream dest_str;
		dest_str << "-> 0x" << std::hex << std::setfill('0') << std::setw(address_width) << destination;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_YELLOW>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>(dest_str.str());
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void LocalOrCellVariableInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// offset " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void GlobalVariableWideInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int operand = (high_byte << 8) | low_byte;
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// " + executable.GetGlobalVariable(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void LocalOrCellVariableWideInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int operand = (high_byte << 8) | low_byte;
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// offset " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void AggregateCreateInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) |
			(static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8) |
			(static_cast<int>(executable.ReadByteCode(offset + 3, proc_index)) << 16);
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// element count: " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void CallInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// number of parameters: " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void CallFixedInstruction(std::string_view name, int arity, int& offset)
	{
		offset += 1;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// number of parameters: " + std::to_string(arity));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void CallGlobalWideInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int high_byte = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int low_byte = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int global_index = (high_byte << 8) | low_byte;
		int arity = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(global_index));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(arity));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// global: " + executable.GetGlobalVariable(global_index) + ", params: " + std::to_string(arity));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void CallForeignInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int arity = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int return_type = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(arity));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(return_type));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// params: " + std::to_string(arity) + ", return: ");

		std::string return_type_str = (return_type == 0) ? "primitive" : (return_type == 1) ? "text" : (return_type == 2) ? "array" : "unknown";
		formated_str << return_type_str;
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void CallForeignIndexedInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int ffi_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int arity = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		int return_type = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		offset += 4;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(ffi_index));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(arity));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(return_type));

		std::string ffi_name = (static_cast<size_t>(ffi_index) < VmBuiltins::COUNT)
			? std::string(VmBuiltins::At(static_cast<size_t>(ffi_index)).m_name)
			: "unknown";
		std::string return_type_str = (return_type == 0) ? "primitive" : (return_type == 1) ? "text" : (return_type == 2) ? "array" : "unknown";

		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// " + ffi_name + ", params: " + std::to_string(arity) + ", return: " + return_type_str);
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void MemberInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// member index: " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void DataInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int operand = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		offset += 2;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(operand));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// data size: " + std::to_string(operand));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void JoinWorkerInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		const int ok_tag = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		const int err_tag = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		const int cancelled_tag = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
		const int failed_tag = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
		offset += 5;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::format("{} {} {} {}", ok_tag, err_tag, cancelled_tag, failed_tag));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>(std::format("// tags: Ok {}, Err {}, Cancelled {}, Failed {}", ok_tag, err_tag, cancelled_tag, failed_tag));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void ConstructUnionInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		int size = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
		int tag = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
		offset += 3;
		std::ostringstream formated_str;

		formated_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(size));
		formated_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(tag));
		formated_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// data size: " + std::to_string(size) + ", union tag: " + std::to_string(tag));
		formated_str << '\n';
		Terminal::Print(formated_str.str());
	}

	void SpawnWorkerInstruction(std::string_view name, const VmExecutable& executable, int proc_index, int& offset)
	{
		const int arg_count = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));

		std::ostringstream formatted_str;
		formatted_str << Terminal::Colored<Terminal::Color::BRIGHT_WHITE>(std::string(name));
		formatted_str << " " << Terminal::Colored<Terminal::Color::CYAN>(std::to_string(arg_count));
		formatted_str << "  " << Terminal::Colored<Terminal::Color::DARK_GRAY>("// argc " + std::to_string(arg_count) + ", function on the stack");
		formatted_str << '\n';
		Terminal::Print(formatted_str.str());

		offset += 2;
	}
}

namespace Disassembler
{
	// Forward declaration
	void DisassembleInstruction(const VmExecutable& executable, int proc_index, int& offset);

	void DisassembleBytecodeStream(const VmExecutable& executable, int proc_index, std::string_view proc_name)
	{
		std::ostringstream header;
		header << std::string(95, '=') << "\n";
		header << " " << Terminal::Colored<Terminal::Color::BRIGHT_CYAN>(std::string(proc_name)) << "\n";
		header << std::string(95, '=') << "\n";
		Terminal::Print(header.str());

		int offset = 0;
		while (offset < executable.GetByteCodeSize(proc_index))
		{
			DisassembleInstruction(executable, proc_index, offset);
		}

		std::ostringstream footer;
		footer << Terminal::Colored<Terminal::Color::DARK_GRAY>(std::string(95, '-')) << "\n\n";
		Terminal::Print(footer.str());
	}

	void DisassembleInstruction(const VmExecutable& executable, int proc_index, int& offset)
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
		Terminal::Print(formated_str.str());

		VmOpCode instruction = executable.ReadByteCode(offset, proc_index);
		switch (instruction) 
		{
		case VmOpCode::LOAD_STRING_WIDE:
			LoadStringWideInstruction("LOAD_STRING_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::INTEGER_CONSTANT:
			NumericConstantInstruction(true, "INTEGER_CONSTANT", executable, proc_index, offset);
			break;
		case VmOpCode::FLOAT_CONSTANT:
			NumericConstantInstruction(false, "FLOAT_CONSTANT", executable, proc_index, offset);
			break;
		case VmOpCode::BYTE_CONSTANT:
			ByteConstantInstruction("BYTE_CONSTANT", executable, proc_index, offset);
			break;
		case VmOpCode::WORD_CONSTANT:
			WordConstantInstruction("WORD_CONSTANT", executable, proc_index, offset);
			break;
		case VmOpCode::OP_UNIT:
			SimpleInstruction("OP_UNIT", offset);
			break;
		case VmOpCode::OP_TRUE:
			SimpleInstruction("OP_TRUE", offset);
			break;
		case VmOpCode::OP_FALSE:
			SimpleInstruction("OP_FALSE", offset);
			break;
		case VmOpCode::INT_MINUS_1:
			SimpleInstruction("INT_MINUS_1", offset);
			break;
		case VmOpCode::INT_0:
			SimpleInstruction("INT_0", offset);
			break;
		case VmOpCode::INT_1:
			SimpleInstruction("INT_1", offset);
			break;
		case VmOpCode::INT_2:
			SimpleInstruction("INT_2", offset);
			break;
		case VmOpCode::INT_3:
			SimpleInstruction("INT_3", offset);
			break;
		case VmOpCode::INT_4:
			SimpleInstruction("INT_4", offset);
			break;
		case VmOpCode::INT_5:
			SimpleInstruction("INT_5", offset);
			break;
		case VmOpCode::INT_10:
			SimpleInstruction("INT_10", offset);
			break;
		case VmOpCode::CREATE_ARRAY:
			AggregateCreateInstruction("CREATE_ARRAY", executable, proc_index, offset);
			break;
		case VmOpCode::CREATE_TUPLE:
			AggregateCreateInstruction("CREATE_TUPLE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_ARRAY:
			SimpleInstruction("GET_ARRAY", offset);
			break;
		case VmOpCode::GET_TUPLE:
			SimpleInstruction("GET_TUPLE", offset);
			break;
		case VmOpCode::ADD_BACK_ARRAY:
			SimpleInstruction("ADD_BACK_ARRAY", offset);
			break;
		case VmOpCode::GET_ARRAY_LENGTH:
			SimpleInstruction("GET_ARRAY_LENGTH", offset);
			break;
		case VmOpCode::CREATE_INT_RANGE:
			SimpleInstruction("CREATE_INT_RANGE", offset);
			break;
		case VmOpCode::CREATE_FLOAT_RANGE:
			SimpleInstruction("CREATE_FLOAT_RANGE", offset);
			break;
		case VmOpCode::GET_RANGE_START:
			SimpleInstruction("GET_RANGE_START", offset);
			break;
		case VmOpCode::GET_RANGE_END:
			SimpleInstruction("GET_RANGE_END", offset);
			break;
		case VmOpCode::GET_RANGE_STEP:
			SimpleInstruction("GET_RANGE_STEP", offset);
			break;
		case VmOpCode::INT_TO_FLOAT:
			SimpleInstruction("INT_TO_FLOAT", offset);
			break;
		case VmOpCode::TEXT_TO_FLOAT:
			SimpleInstruction("TEXT_TO_FLOAT", offset);
			break;
		case VmOpCode::FLOAT_TO_INT:
			SimpleInstruction("FLOAT_TO_INT", offset);
			break;
		case VmOpCode::TEXT_TO_INT:
			SimpleInstruction("TEXT_TO_INT", offset);
			break;
		case VmOpCode::FLOAT_TO_TEXT:
			SimpleInstruction("FLOAT_TO_TEXT", offset);
			break;
		case VmOpCode::INT_TO_TEXT:
			SimpleInstruction("INT_TO_TEXT", offset);
			break;
		case VmOpCode::WORD_TO_TEXT:
			SimpleInstruction("WORD_TO_TEXT", offset);
			break;
		case VmOpCode::BYTE_TO_INT:
			SimpleInstruction("BYTE_TO_INT", offset);
			break;
		case VmOpCode::INT_TO_BYTE:
			SimpleInstruction("INT_TO_BYTE", offset);
			break;
		case VmOpCode::BYTE_TO_WORD:
			SimpleInstruction("BYTE_TO_WORD", offset);
			break;
		case VmOpCode::WORD_TO_BYTE:
			SimpleInstruction("WORD_TO_BYTE", offset);
			break;
		case VmOpCode::WORD_TO_INT:
			SimpleInstruction("WORD_TO_INT", offset);
			break;
		case VmOpCode::INT_TO_WORD:
			SimpleInstruction("INT_TO_WORD", offset);
			break;
		case VmOpCode::BYTE_TO_FLOAT:
			SimpleInstruction("BYTE_TO_FLOAT", offset);
			break;
		case VmOpCode::FLOAT_TO_BYTE:
			SimpleInstruction("FLOAT_TO_BYTE", offset);
			break;
		case VmOpCode::WORD_TO_FLOAT:
			SimpleInstruction("WORD_TO_FLOAT", offset);
			break;
		case VmOpCode::FLOAT_TO_WORD:
			SimpleInstruction("FLOAT_TO_WORD", offset);
			break;
		case VmOpCode::LEFT_SHIFT:
			SimpleInstruction("LEFT_SHIFT", offset);
			break;
		case VmOpCode::RIGHT_SHIFT:
			SimpleInstruction("RIGHT_SHIFT", offset);
			break;
		case VmOpCode::LEFT_SHIFT_BYTE:
			SimpleInstruction("LEFT_SHIFT_BYTE", offset);
			break;
		case VmOpCode::RIGHT_SHIFT_BYTE:
			SimpleInstruction("RIGHT_SHIFT_BYTE", offset);
			break;
		case VmOpCode::LEFT_SHIFT_WORD:
			SimpleInstruction("LEFT_SHIFT_WORD", offset);
			break;
		case VmOpCode::RIGHT_SHIFT_WORD:
			SimpleInstruction("RIGHT_SHIFT_WORD", offset);
			break;
		case VmOpCode::BITWISE_AND:
			SimpleInstruction("BITWISE_AND", offset);
			break;
		case VmOpCode::BITWISE_OR:
			SimpleInstruction("BITWISE_OR", offset);
			break;
		case VmOpCode::BITWISE_XOR:
			SimpleInstruction("BITWISE_XOR", offset);
			break;
		case VmOpCode::BITWISE_NOT:
			SimpleInstruction("BITWISE_NOT", offset);
			break;
		case VmOpCode::ADD_FLOAT:
			SimpleInstruction("ADD_FLOAT", offset);
			break;
		case VmOpCode::SUBTRACT_FLOAT:
			SimpleInstruction("SUBTRACT_FLOAT", offset);
			break;
		case VmOpCode::MULTIPLY_FLOAT:
			SimpleInstruction("MULTIPLY_FLOAT", offset);
			break;
		case VmOpCode::DIVIDE_FLOAT:
			SimpleInstruction("DIVIDE_FLOAT", offset);
			break;
		case VmOpCode::MODULO_FLOAT:
			SimpleInstruction("MODULO_FLOAT", offset);
			break;
		case VmOpCode::ADD_INTEGER:
			SimpleInstruction("ADD_INTEGER", offset);
			break;
		case VmOpCode::SUBTRACT_INTEGER:
			SimpleInstruction("SUBTRACT_INTEGER", offset);
			break;
		case VmOpCode::MULTIPLY_INTEGER:
			SimpleInstruction("MULTIPLY_INTEGER", offset);
			break;
		case VmOpCode::DIVIDE_INTEGER:
			SimpleInstruction("DIVIDE_INTEGER", offset);
			break;
		case VmOpCode::MODULO_INTEGER:
			SimpleInstruction("MODULO_INTEGER", offset);
			break;
		case VmOpCode::ADD_BYTE:
			SimpleInstruction("ADD_BYTE", offset);
			break;
		case VmOpCode::SUBTRACT_BYTE:
			SimpleInstruction("SUBTRACT_BYTE", offset);
			break;
		case VmOpCode::MULTIPLY_BYTE:
			SimpleInstruction("MULTIPLY_BYTE", offset);
			break;
		case VmOpCode::DIVIDE_BYTE:
			SimpleInstruction("DIVIDE_BYTE", offset);
			break;
		case VmOpCode::MODULO_BYTE:
			SimpleInstruction("MODULO_BYTE", offset);
			break;
		case VmOpCode::ADD_WORD:
			SimpleInstruction("ADD_WORD", offset);
			break;
		case VmOpCode::SUBTRACT_WORD:
			SimpleInstruction("SUBTRACT_WORD", offset);
			break;
		case VmOpCode::MULTIPLY_WORD:
			SimpleInstruction("MULTIPLY_WORD", offset);
			break;
		case VmOpCode::DIVIDE_WORD:
			SimpleInstruction("DIVIDE_WORD", offset);
			break;
		case VmOpCode::MODULO_WORD:
			SimpleInstruction("MODULO_WORD", offset);
			break;
		case VmOpCode::CONCAT_ARRAY:
			SimpleInstruction("CONCAT_ARRAY", offset);
			break;
		case VmOpCode::CONCAT_TEXT:
			SimpleInstruction("CONCAT_TEXT", offset);
			break;
		case VmOpCode::EQUAL_FLOAT:
			SimpleInstruction("EQUAL_FLOAT", offset);
			break;
		case VmOpCode::NOT_EQUAL_FLOAT:
			SimpleInstruction("NOT_EQUAL_FLOAT", offset);
			break;
		case VmOpCode::GREATER_FLOAT:
			SimpleInstruction("GREATER_FLOAT", offset);
			break;
		case VmOpCode::GREATER_EQUAL_FLOAT:
			SimpleInstruction("GREATER_EQUAL_FLOAT", offset);
			break;
		case VmOpCode::LESS_FLOAT:
			SimpleInstruction("LESS_FLOAT", offset);
			break;
		case VmOpCode::LESS_EQUAL_FLOAT:
			SimpleInstruction("LESS_EQUAL_FLOAT", offset);
			break;
		case VmOpCode::EQUAL_INTEGER:
			SimpleInstruction("EQUAL_INTEGER", offset);
			break;
		case VmOpCode::NOT_EQUAL_INTEGER:
			SimpleInstruction("NOT_EQUAL_INTEGER", offset);
			break;
		case VmOpCode::GREATER_INTEGER:
			SimpleInstruction("GREATER_INTEGER", offset);
			break;
		case VmOpCode::GREATER_EQUAL_INTEGER:
			SimpleInstruction("GREATER_EQUAL_INTEGER", offset);
			break;
		case VmOpCode::LESS_INTEGER:
			SimpleInstruction("LESS_INTEGER", offset);
			break;
		case VmOpCode::LESS_EQUAL_INTEGER:
			SimpleInstruction("LESS_EQUAL_INTEGER", offset);
			break;
		case VmOpCode::EQUAL_BYTE:
			SimpleInstruction("EQUAL_BYTE", offset);
			break;
		case VmOpCode::NOT_EQUAL_BYTE:
			SimpleInstruction("NOT_EQUAL_BYTE", offset);
			break;
		case VmOpCode::GREATER_BYTE:
			SimpleInstruction("GREATER_BYTE", offset);
			break;
		case VmOpCode::GREATER_EQUAL_BYTE:
			SimpleInstruction("GREATER_EQUAL_BYTE", offset);
			break;
		case VmOpCode::LESS_BYTE:
			SimpleInstruction("LESS_BYTE", offset);
			break;
		case VmOpCode::LESS_EQUAL_BYTE:
			SimpleInstruction("LESS_EQUAL_BYTE", offset);
			break;
		case VmOpCode::EQUAL_WORD:
			SimpleInstruction("EQUAL_WORD", offset);
			break;
		case VmOpCode::NOT_EQUAL_WORD:
			SimpleInstruction("NOT_EQUAL_WORD", offset);
			break;
		case VmOpCode::GREATER_WORD:
			SimpleInstruction("GREATER_WORD", offset);
			break;
		case VmOpCode::GREATER_EQUAL_WORD:
			SimpleInstruction("GREATER_EQUAL_WORD", offset);
			break;
		case VmOpCode::LESS_WORD:
			SimpleInstruction("LESS_WORD", offset);
			break;
		case VmOpCode::LESS_EQUAL_WORD:
			SimpleInstruction("LESS_EQUAL_WORD", offset);
			break;
		case VmOpCode::EQUAL_TEXT:
			SimpleInstruction("EQUAL_TEXT", offset);
			break;
		case VmOpCode::NOT:
			SimpleInstruction("NOT", offset);
			break;
		case VmOpCode::NEGATE_FLOAT:
			SimpleInstruction("NEGATE_FLOAT", offset);
			break;
		case VmOpCode::NEGATE_INTEGER:
			SimpleInstruction("NEGATE_INTEGER", offset);
			break;
		case VmOpCode::JUMP_IF_FALSE:
			JumpInstruction("JUMP_IF_FALSE", 1, executable, proc_index, offset);
			break;
		case VmOpCode::JUMP:
			JumpInstruction("JUMP", 1, executable, proc_index, offset);
			break;
		case VmOpCode::JUMP_BACK:
			JumpInstruction("JUMP_BACK", -1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_INTEGER_LESS:
			JumpInstruction("IF_INTEGER_LESS", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_INTEGER_LESS_EQUAL:
			JumpInstruction("IF_INTEGER_LESS_EQUAL", 1, executable, proc_index, offset);
				break;
		case VmOpCode::IF_INTEGER_GREATER:
			JumpInstruction("IF_INTEGER_GREATER", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_INTEGER_GREATER_EQUAL:
			JumpInstruction("IF_INTEGER_GREATER_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_INTEGER_EQUAL:
			JumpInstruction("IF_INTEGER_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_INTEGER_NOT_EQUAL:
			JumpInstruction("IF_INTEGER_NOT_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_LESS:
			JumpInstruction("IF_FLOAT_LESS", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_LESS_EQUAL:
			JumpInstruction("IF_FLOAT_LESS_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_GREATER:
			JumpInstruction("IF_FLOAT_GREATER", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_GREATER_EQUAL:
			JumpInstruction("IF_FLOAT_GREATER_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_EQUAL:
			JumpInstruction("IF_FLOAT_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::IF_FLOAT_NOT_EQUAL:
			JumpInstruction("IF_FLOAT_NOT_EQUAL", 1, executable, proc_index, offset);
			break;
		case VmOpCode::GET_TAG:
			SimpleInstruction("GET_TAG", offset);
			break;
		case VmOpCode::SPAWN_WORKER:
			SpawnWorkerInstruction("SPAWN_WORKER", executable, proc_index, offset);
			break;
		case VmOpCode::JOIN_WORKER:
			JoinWorkerInstruction("JOIN_WORKER", executable, proc_index, offset);
			break;
		case VmOpCode::CHANNEL_CREATE:
			SimpleInstruction("CHANNEL_CREATE", offset);
			break;
		case VmOpCode::CHANNEL_SEND:
			SimpleInstruction("CHANNEL_SEND", offset);
			break;
		case VmOpCode::CHANNEL_RECEIVE:
			SimpleInstruction("CHANNEL_RECEIVE", offset);
			break;
		case VmOpCode::CHANNEL_CLOSE:
			SimpleInstruction("CHANNEL_CLOSE", offset);
			break;
		case VmOpCode::WORKER_IS_DONE:
			SimpleInstruction("WORKER_IS_DONE", offset);
			break;
		case VmOpCode::MAKE_CELL:
			SimpleInstruction("MAKE_CELL", offset);
			break;
		case VmOpCode::READ_CELL:
			SimpleInstruction("READ_CELL", offset);
			break;
		case VmOpCode::WRITE_CELL:
			SimpleInstruction("WRITE_CELL", offset);
			break;
		case VmOpCode::WORKER_CANCEL:
			SimpleInstruction("WORKER_CANCEL", offset);
			break;
		case VmOpCode::ADD_LOCAL_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("ADD_LOCAL_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::PUSH_LOCAL_SUB_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("PUSH_LOCAL_SUB_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::IF_LOCAL_LE_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_LE_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::IF_LOCAL_GE_LOCAL:
		{
			const int left = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int right = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_GE_LOCAL")) + " left=" + std::to_string(left) + " right=" + std::to_string(right) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::GET_LOCAL2:
		{
			const int first = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int second = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("GET_LOCAL2")) + " first=" + std::to_string(first) + " second=" + std::to_string(second) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::STORE_LOCAL:
			LocalOrCellVariableInstruction("STORE_LOCAL", executable, proc_index, offset);
			break;
		case VmOpCode::IF_LOCAL_TAG_NOT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int tag = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_TAG_NOT")) + " local=" + std::to_string(local_index) + " tag=" + std::to_string(tag) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::LOCAL_UNION_FIELD:
		{
			const int union_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int field_index = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int target_index = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("LOCAL_UNION_FIELD")) + " union=" + std::to_string(union_index) + " index=" + std::to_string(field_index) + " target=" + std::to_string(target_index) + "\n");
			offset += 4;
			break;
		}
		case VmOpCode::IF_LOCAL_LT_INT:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_LT_INT")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::IF_LOCAL_LT_LOCAL:
		{
			const int left = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int right = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_LT_LOCAL")) + " left=" + std::to_string(left) + " right=" + std::to_string(right) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::IF_LOCAL_EQ_LOCAL:
		{
			const int left = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int right = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int low = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int high = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("IF_LOCAL_EQ_LOCAL")) + " left=" + std::to_string(left) + " right=" + std::to_string(right) + " -> " + std::to_string(offset + 5 + (low | (high << 8))) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::STEP_LOCAL:
		{
			const int local_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int imm = static_cast<int>(static_cast<int8_t>(executable.ReadByteCode(offset + 2, proc_index)));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("STEP_LOCAL")) + " local=" + std::to_string(local_index) + " imm=" + std::to_string(imm) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::LOCAL_ARRAY_GET:
		{
			const int array_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int index = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int target_index = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("LOCAL_ARRAY_GET")) + " array=" + std::to_string(array_index) + " index=" + std::to_string(index) + " target=" + std::to_string(target_index) + "\n");
			offset += 4;
			break;
		}
		case VmOpCode::LOCAL_UNION2:
		{
			const int tag = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int first = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			const int second = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			const int target_index = static_cast<int>(executable.ReadByteCode(offset + 4, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("LOCAL_UNION2")) + " tag=" + std::to_string(tag) + " first=" + std::to_string(first) + " second=" + std::to_string(second) + " target=" + std::to_string(target_index) + "\n");
			offset += 5;
			break;
		}
		case VmOpCode::APPEND_LOCAL:
		{
			const int array_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			const int value_index = static_cast<int>(executable.ReadByteCode(offset + 2, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("APPEND_LOCAL")) + " array=" + std::to_string(array_index) + " value=" + std::to_string(value_index) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::MAKE_CLOSURE_OF:
		{
			const int code_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) | (static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8);
			const int count = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("MAKE_CLOSURE_OF")) + " code=" + std::to_string(code_index) + " captures=" + std::to_string(count) + "\n");
			offset += 4;
			break;
		}
		case VmOpCode::CALL_PROC_WIDE:
		{
			const int code_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) | (static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8);
			const int arity = static_cast<int>(executable.ReadByteCode(offset + 3, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("CALL_PROC_WIDE")) + " code=" + std::to_string(code_index) + " arity=" + std::to_string(arity) + "\n");
			offset += 4;
			break;
		}
		case VmOpCode::MAKE_FUNCTION_WIDE:
		{
			const int code_index = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index)) | (static_cast<int>(executable.ReadByteCode(offset + 2, proc_index)) << 8);
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("MAKE_FUNCTION_WIDE")) + " code=" + std::to_string(code_index) + "\n");
			offset += 3;
			break;
		}
		case VmOpCode::SET_CAPTURE:
			MemberInstruction("SET_CAPTURE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_UNION_FIELD:
			MemberInstruction("GET_UNION_FIELD", executable, proc_index, offset);
			break;
		case VmOpCode::EXTEND_ARRAY:
			SimpleInstruction("EXTEND_ARRAY", offset);
			break;
		case VmOpCode::EXTEND_TEXT:
			SimpleInstruction("EXTEND_TEXT", offset);
			break;
		case VmOpCode::CALL_FOREIGN:
			CallForeignInstruction("CALL_FOREIGN", executable, proc_index, offset);
			break;
		case VmOpCode::CALL_FOREIGN_INDEXED:
			CallForeignIndexedInstruction("CALL_FOREIGN_INDEXED", executable, proc_index, offset);
			break;
		case VmOpCode::CALL:
			CallInstruction("CALL", executable, proc_index, offset);
			break;
		case VmOpCode::CALL_0:
			CallFixedInstruction("CALL_0", 0, offset);
			break;
		case VmOpCode::CALL_1:
			CallFixedInstruction("CALL_1", 1, offset);
			break;
		case VmOpCode::CALL_2:
			CallFixedInstruction("CALL_2", 2, offset);
			break;
		case VmOpCode::CALL_3:
			CallFixedInstruction("CALL_3", 3, offset);
			break;
		case VmOpCode::TAIL_CALL:
			CallInstruction("TAIL_CALL", executable, proc_index, offset);
			break;
		case VmOpCode::CONSTRUCT_STRUCT:
			DataInstruction("CONSTRUCT_STRUCT", executable, proc_index, offset);
			break;
		case VmOpCode::CONSTRUCT_UNION:
			ConstructUnionInstruction("CONSTRUCT_UNION", executable, proc_index, offset);
			break;
		case VmOpCode::LOAD_EMPTY_UNION:
			DataInstruction("LOAD_EMPTY_UNION", executable, proc_index, offset);
			break;
		case VmOpCode::GET_LOCAL:
			LocalOrCellVariableInstruction("GET_LOCAL", executable, proc_index, offset);
			break;
		case VmOpCode::SET_LOCAL:
			LocalOrCellVariableInstruction("SET_LOCAL", executable, proc_index, offset);
			break;
		case VmOpCode::GET_CELL:
			LocalOrCellVariableInstruction("GET_CELL", executable, proc_index, offset);
			break;
		case VmOpCode::DEFINE_GLOBAL_WIDE:
			GlobalVariableWideInstruction("DEFINE_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_GLOBAL_WIDE:
			GlobalVariableWideInstruction("GET_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::SET_GLOBAL_WIDE:
			GlobalVariableWideInstruction("SET_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::CALL_GLOBAL_WIDE:
			CallGlobalWideInstruction("CALL_GLOBAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_LOCAL_WIDE:
			LocalOrCellVariableWideInstruction("GET_LOCAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::SET_LOCAL_WIDE:
			LocalOrCellVariableWideInstruction("SET_LOCAL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_CELL_WIDE:
			LocalOrCellVariableWideInstruction("GET_CELL_WIDE", executable, proc_index, offset);
			break;
		case VmOpCode::GET_MEMBER:
			MemberInstruction("GET_MEMBER", executable, proc_index, offset);
			break;
		case VmOpCode::POP:
			SimpleInstruction("POP", offset);
			break;
		case VmOpCode::RETURN:
			SimpleInstruction("RETURN", offset);
			break;
		case VmOpCode::HALT:
			SimpleInstruction("HALT", offset);
			break;
		case VmOpCode::PUSH_PLACEHOLDER:
		{
			const int count = static_cast<int>(executable.ReadByteCode(offset + 1, proc_index));
			Terminal::Print(std::string(Terminal::Colored<Terminal::Color::BRIGHT_WHITE>("PUSH_PLACEHOLDER")) + " count=" + std::to_string(count) + "\n");
			offset += 2;
			break;
		}
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
