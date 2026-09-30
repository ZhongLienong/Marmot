#pragma once

#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Lowering/Lowering.h"
#include "Compiler/Result/Result.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// A lowered module to bytecode, for the linker.
// The module's top-level function is procedure 0 and every other function
// follows in order. A value that crosses a block edge or is used twice lives
// in a frame slot; a value used once, by an instruction that can take it from
// the stack where it was made, stays there. Its only diagnostics are the
// bytecode encoding's limits.
class BytecodeBackend
{
private:
	const LoweredModule& m_lowered;
	std::string m_file_name;
	const std::vector<std::string>& m_source_lines;

	std::vector<size_t> m_procedure_indices;
	// A global's operand: its index among this module's globals, or an
	// import placeholder the linker resolves.
	std::vector<int> m_global_operands;
	std::vector<std::string> m_string_pool;
	std::unordered_map<std::string, int> m_string_indices;

public:
	BytecodeBackend(const LoweredModule& lowered, std::string_view file_name, const std::vector<std::string>& source_lines);

	MidoriResult::BytecodeBackendResult Emit() &&;

	[[nodiscard]] std::optional<BytecodeModule::SourceProvenance> MakeSourceProvenance(const Token& token) const;
	[[nodiscard]] CompilerError LimitExceeded(std::string_view message, int line) const;
	size_t ProcedureIndex(MidoriIRFunctionId function) const;
	size_t CaptureCount(MidoriIRFunctionId function) const;
	int GlobalOperand(MidoriIRGlobalSlot slot) const;
	std::expected<int, CompilerError> TextConstant(const std::string& text, int line);

private:
	std::expected<std::vector<BytecodeModule::ImportedSymbol>, CompilerError> AssignGlobals();
};
