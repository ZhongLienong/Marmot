#pragma once

#include "Common/Value/Value.h"

#include <array>
#include <cinttypes>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

enum class OpCode : uint8_t
{
#define MARMOT_OPCODE(name, length) name,
#include "Common/Executable/OpCodes.def"
#undef MARMOT_OPCODE
};

// Facts about each instruction, from the same list as the enum.
class OpCodeTable
{
private:
	inline static constexpr std::array<int, 256> s_lengths = []()
	{
		std::array<int, 256> lengths{};
		size_t index = 0uz;
#define MARMOT_OPCODE(name, length) lengths[index] = length; index += 1uz;
#include "Common/Executable/OpCodes.def"
#undef MARMOT_OPCODE
		return lengths;
	}();

	inline static constexpr std::array s_names =
	{
#define MARMOT_OPCODE(name, length) std::string_view(#name),
#include "Common/Executable/OpCodes.def"
#undef MARMOT_OPCODE
	};

public:
	static constexpr size_t COUNT = s_names.size();
	static_assert(COUNT <= 256uz, "opcodes are one byte");

	// The whole instruction in bytes, the opcode byte included. 0 for a byte
	// value that is not an opcode.
	static constexpr int Length(OpCode opcode)
	{
		return s_lengths[static_cast<size_t>(opcode)];
	}

	static constexpr std::string_view Name(OpCode opcode)
	{
		const size_t index = static_cast<size_t>(opcode);
		return index < COUNT ? s_names[index] : std::string_view("<invalid opcode>");
	}
};

class BytecodeStream
{
public:
	using iterator = std::vector<OpCode>::iterator;
	using const_iterator = std::vector<OpCode>::const_iterator;
	using reverse_iterator = std::vector<OpCode>::reverse_iterator;
	using const_reverse_iterator = std::vector<OpCode>::const_reverse_iterator;

	iterator begin();
	iterator end();
	const_iterator cbegin() const;
	const_iterator cend() const;
	reverse_iterator rbegin();
	reverse_iterator rend();
	const_reverse_iterator crbegin() const;
	const_reverse_iterator crend() const;

private:
	std::vector<OpCode> m_bytecode;
	std::vector<std::pair<int, int>> m_line_info; // Pair of line number and count of consecutive instructions

public:

	OpCode ReadByteCode(int index) const;

	void SetByteCode(int index, OpCode byte);

	void AddByteCode(OpCode byte, int line);

	void PopByteCode(int line);

	int GetByteCodeSize() const;

	bool IsByteCodeEmpty() const;

	int GetLine(int index) const;

	void Append(BytecodeStream&& other);

	const OpCode* operator[](int index) const;

	const std::vector<std::pair<int, int>>& GetLineInfo() const;

	static BytecodeStream FromRaw(std::vector<OpCode>&& bytecode, std::vector<std::pair<int, int>>&& line_info);
};

// How a native library may be used, as the package that ships it declares.
struct NativeLibraryPolicy
{
	// Verified before the library loads.
	std::optional<std::string> m_checksum = std::nullopt;
	// Whether workers may call it concurrently.
	bool m_thread_safe = false;
};

// A native library the program calls into. Symbols are what the program's
// `foreign ... from "library"` declarations name; hint directories are the
// directories of the modules that declared them, searched after the library
// path when the program runs.
struct NativeLibraryImport
{
	std::string m_name;
	std::vector<std::string> m_symbols;
	std::vector<std::string> m_hint_directories;
	NativeLibraryPolicy m_policy;
};

// The value a library foreign function holds: its library and symbol, joined by
// NATIVE_SYMBOL_SEPARATOR. A builtin's value is its bare name.
inline constexpr char NATIVE_SYMBOL_SEPARATOR = '\x1f';

class MidoriExecutable
{
public:
	using GlobalNames = std::vector<std::string>;
	using Procedures = std::vector<BytecodeStream>;
	using ProcedureSourcePaths = std::vector<std::string>;
	using StringPool = std::vector<std::string>;
	using SourceFileTable = std::unordered_map<std::string, std::vector<std::string>>;
	std::vector<std::string> m_procedure_names;
	std::string m_file_name;

private:
	GlobalNames m_globals;
	Procedures m_procedures;
	ProcedureSourcePaths m_procedure_source_paths;
	StringPool m_string_pool;
	SourceFileTable m_source_files;
	std::vector<NativeLibraryImport> m_native_libraries;

public:

	int AddGlobalVariable(std::string&& name);

	const std::string& GetGlobalVariable(int index) const;

	void AttachProcedures(Procedures&& bytecode);

	void AddStringPool(StringPool&& string_pool);

	void AttachProcedureNames(std::vector<std::string>&& procedure_names);

	void AttachProcedureSourcePaths(ProcedureSourcePaths&& procedure_source_paths);

	void AttachSourceFiles(SourceFileTable&& source_files);

	void SetFileName(std::string&& file_name);

	std::string_view GetFileName() const;

	std::string_view GetProcedureSourcePath(int proc_index) const;

	int GetLine(int instr_index, int proc_index) const;

	const BytecodeStream& GetBytecodeStream(int proc_index) const;

	OpCode ReadByteCode(int instr_index, int proc_index) const;

	int GetByteCodeSize(int proc_index) const;

	int GetProcedureCount() const;

	int GetGlobalVariableCount() const;

	const StringPool& GetStringPool() const;

	const std::vector<std::string>* FindSourceLines(std::string_view file_name) const;

	void AttachNativeLibraries(std::vector<NativeLibraryImport>&& native_libraries);

	const std::vector<NativeLibraryImport>& GetNativeLibraries() const;
};
