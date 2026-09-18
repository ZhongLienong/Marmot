#pragma once

#include "Common/Value/Value.h"

#include <cinttypes>
#include <string>
#include <unordered_map>
#include <vector>

enum class OpCode : uint8_t
{
	// Constants and Literals
	LOAD_STRING,
	LOAD_STRING_WIDE,
	INTEGER_CONSTANT,
	FLOAT_CONSTANT,
	BYTE_CONSTANT,
	WORD_CONSTANT,
	OP_UNIT,
	OP_TRUE,
	OP_FALSE,

	// Small integer constants (optimization for common values)
	INT_MINUS_1,
	INT_0,
	INT_1,
	INT_2,
	INT_3,
	INT_4,
	INT_5,
	INT_10,

	// Array Operations
	CREATE_ARRAY,
	CREATE_TUPLE,
	GET_ARRAY,
	GET_TUPLE,
	UNPACK_TUPLE,
	ADD_BACK_ARRAY,
	GET_ARRAY_LENGTH,

	// Range Operations
	CREATE_INT_RANGE,
	CREATE_FLOAT_RANGE,
	GET_RANGE_START,
	GET_RANGE_END,
	GET_RANGE_STEP,

	// Atomic type casting
	INT_TO_FLOAT,
	TEXT_TO_FLOAT,
	FLOAT_TO_INT,
	TEXT_TO_INT,
	FLOAT_TO_TEXT,
	INT_TO_TEXT,
	WORD_TO_TEXT,
	BYTE_TO_INT,
	INT_TO_BYTE,
	BYTE_TO_WORD,
	WORD_TO_BYTE,
	WORD_TO_INT,
	INT_TO_WORD,
	BYTE_TO_FLOAT,
	FLOAT_TO_BYTE,
	WORD_TO_FLOAT,
	FLOAT_TO_WORD,

	// Bit Operations
	LEFT_SHIFT,
	RIGHT_SHIFT,
	LEFT_SHIFT_BYTE,
	RIGHT_SHIFT_BYTE,
	LEFT_SHIFT_WORD,
	RIGHT_SHIFT_WORD,
	BITWISE_AND,
	BITWISE_OR,
	BITWISE_XOR,
	BITWISE_NOT,

	// Arithmetic Operations
	ADD_FLOAT,
	SUBTRACT_FLOAT,
	MULTIPLY_FLOAT,
	DIVIDE_FLOAT,
	MODULO_FLOAT,
	ADD_INTEGER,
	SUBTRACT_INTEGER,
	MULTIPLY_INTEGER,
	DIVIDE_INTEGER,
	MODULO_INTEGER,
	ADD_BYTE,
	SUBTRACT_BYTE,
	MULTIPLY_BYTE,
	DIVIDE_BYTE,
	MODULO_BYTE,
	ADD_WORD,
	SUBTRACT_WORD,
	MULTIPLY_WORD,
	DIVIDE_WORD,
	MODULO_WORD,

	// Concatenations
	CONCAT_ARRAY,
	CONCAT_TEXT,

	// Compound Assignment Operations
	ADD_ASSIGN_INT,
	SUB_ASSIGN_INT,

	// Comparison Operations
	EQUAL_FLOAT,
	NOT_EQUAL_FLOAT,
	GREATER_FLOAT,
	GREATER_EQUAL_FLOAT,
	LESS_FLOAT,
	LESS_EQUAL_FLOAT,
	EQUAL_INTEGER,
	NOT_EQUAL_INTEGER,
	GREATER_INTEGER,
	GREATER_EQUAL_INTEGER,
	LESS_INTEGER,
	LESS_EQUAL_INTEGER,
	EQUAL_BYTE,
	NOT_EQUAL_BYTE,
	GREATER_BYTE,
	GREATER_EQUAL_BYTE,
	LESS_BYTE,
	LESS_EQUAL_BYTE,
	EQUAL_WORD,
	NOT_EQUAL_WORD,
	GREATER_WORD,
	GREATER_EQUAL_WORD,
	LESS_WORD,
	LESS_EQUAL_WORD,
	EQUAL_TEXT,

	// Logical Operations
	NOT,

	// UnaryPrefix Operations
	NEGATE_FLOAT,
	NEGATE_INTEGER,

	// Control Flow
	JUMP_IF_FALSE,
	JUMP_IF_TRUE,
	JUMP,
	JUMP_BACK,
	IF_INTEGER_LESS,
	IF_INTEGER_LESS_EQUAL,
	IF_INTEGER_GREATER,
	IF_INTEGER_GREATER_EQUAL,
	IF_INTEGER_EQUAL,
	IF_INTEGER_NOT_EQUAL,
	IF_FLOAT_LESS,
	IF_FLOAT_LESS_EQUAL,
	IF_FLOAT_GREATER,
	IF_FLOAT_GREATER_EQUAL,
	IF_FLOAT_EQUAL,
	IF_FLOAT_NOT_EQUAL,

	// Match
	LOAD_TAG,
	GET_TAG,
	SET_TAG,

	// Callable
	CALL_FOREIGN,
	CALL_FOREIGN_INDEXED,
	CALL,
	CALL_0,
	CALL_1,
	CALL_2,
	CALL_3,
	CALL_PROC,
	CALL_PROC_0,
	CALL_PROC_1,
	CALL_PROC_2,
	CALL_PROC_3,
	CALL_GLOBAL,
	CALL_GLOBAL_WIDE,
	TAIL_CALL,
	CONSTRUCT_STRUCT,
	CONSTRUCT_UNION,
	LOAD_EMPTY_UNION,

	// Closure Operations
	MAKE_CLOSURE,
	BIND_CAPTURES,
	MAKE_FUNCTION,
	DEFINE_GLOBAL,
	GET_GLOBAL,
	SET_GLOBAL,
	GET_LOCAL,
	SET_LOCAL,
	GET_LOCAL_CELL,
	SET_LOCAL_CELL,
	GET_CELL,
	SET_CELL,

	// Cell<T>: operate on a cell value on the stack, not on a captured local.
	MAKE_CELL,
	READ_CELL,
	WRITE_CELL,

	// Wide variable operations (for indices > 255)
	DEFINE_GLOBAL_WIDE,
	GET_GLOBAL_WIDE,
	SET_GLOBAL_WIDE,
	GET_LOCAL_WIDE,
	SET_LOCAL_WIDE,
	GET_LOCAL_CELL_WIDE,
	SET_LOCAL_CELL_WIDE,
	GET_CELL_WIDE,
	SET_CELL_WIDE,

	// Struct Operations
	GET_MEMBER,

	// Stack Operations
	POP,
	DUP,
	SWAP,
	POP_LOCAL_SCOPE,
	POP_VALUES,
	POP_BLOCK_SCOPE,
	POP_MATCH_SCOPE,

	// Return
	RETURN,
	HALT,

	// Placeholder
	PUSH_PLACEHOLDER,

	// Concurrency
	SPAWN_WORKER,
	JOIN_WORKER,
	CHANNEL_CREATE,
	CHANNEL_SEND,
	CHANNEL_RECEIVE,
	CHANNEL_CLOSE,
	WORKER_IS_DONE,
	WORKER_CANCEL,

	// In-place concatenation
	EXTEND_ARRAY,
	EXTEND_TEXT,

	// Fused local-operand superinstructions. Byte layouts are padded so
	// RewriteEmittedLocalOps can rewrite them into the unfused sequence in
	// place when a local is promoted to a cell.
	ADD_LOCAL_INT,      // [op][local][imm8][pad][pad][local]           local += imm, push result
	PUSH_LOCAL_SUB_INT, // [op][local][imm8][pad]                       push local - imm
	IF_LOCAL_LE_INT,    // [op][local][imm8][pad][off_lo][off_hi]       branch if !(local <= imm)
	IF_LOCAL_GE_LOCAL,  // [op][left][pad][right][pad][off_lo][off_hi]  branch if !(left >= right)
	GET_LOCAL2,         // [op][first][pad][second]                     push two locals
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
};
