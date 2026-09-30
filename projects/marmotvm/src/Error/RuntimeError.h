#pragma once

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class RuntimeErrorCode
{
	None,
	IndexOutOfBounds,
	NegativeArraySize,
	ArraySizeExceeded,
	FFIFunctionNotFound,
	StackOverflow,
	MemoryAccessViolation,
	DivisionByZero,
	InternalTypeError,
	InternalFFITypeError,
	UnsupportedPlatformOperation,
	WorkerCancelled,
	WorkerExited,
	InvalidConversion
};

enum class RuntimeDiagnosticKind
{
	Error,
	Panic
};

struct RuntimeErrorLocation
{
	std::string m_file_name;
	int m_line = 0;
	std::optional<int> m_column = std::nullopt;
	std::optional<size_t> m_caret_length = std::nullopt;
	std::optional<int> m_end_line = std::nullopt;
	std::optional<int> m_end_column = std::nullopt;
	std::optional<std::string> m_source_line = std::nullopt;
};

struct RuntimeStackFrame
{
	std::string m_procedure_name;
	std::string m_module_name;
	RuntimeErrorLocation m_location;
	int m_recursive_call_count = 1;
};

struct RuntimeError
{
	RuntimeDiagnosticKind m_kind = RuntimeDiagnosticKind::Error;
	RuntimeErrorCode m_code = RuntimeErrorCode::None;
	std::string m_message;
	std::optional<RuntimeErrorLocation> m_location = std::nullopt;
	std::vector<RuntimeStackFrame> m_stack;
	std::string m_rendered;

	RuntimeError(RuntimeErrorCode code, std::string_view message, std::optional<RuntimeErrorLocation> location = std::nullopt, std::vector<RuntimeStackFrame>&& stack = {}, std::optional<RuntimeDiagnosticKind> kind = std::nullopt);

	[[nodiscard]] int ExitCode() const;
	[[nodiscard]] std::string_view Rendered() const;
};

[[nodiscard]] std::string_view RuntimeErrorCodeName(RuntimeErrorCode code);
[[nodiscard]] std::string SerializeMachineReadableRuntimeError(const RuntimeError& error);

namespace std
{
	template<>
	struct formatter<RuntimeError> : formatter<std::string_view>
	{
		auto format(const RuntimeError& error, format_context& ctx) const
		{
			return formatter<std::string_view>::format(error.Rendered(), ctx);
		}
	};
}
