#include "RuntimeError.h"
#include "Support/Json/Json.h"
#include "Support/Terminal/Terminal.h"
#include <algorithm>
#include <cstdlib>
#include <print>
#include <sstream>

std::string_view RuntimeErrorCodeName(RuntimeErrorCode code)
{
	switch (code)
	{
	case RuntimeErrorCode::None:
		return "None";
	case RuntimeErrorCode::IndexOutOfBounds:
		return "IndexOutOfBounds";
	case RuntimeErrorCode::FFIFunctionNotFound:
		return "FFIFunctionNotFound";
	case RuntimeErrorCode::StackOverflow:
		return "StackOverflow";
	case RuntimeErrorCode::MemoryAccessViolation:
		return "MemoryAccessViolation";
	case RuntimeErrorCode::DivisionByZero:
		return "DivisionByZero";
	case RuntimeErrorCode::InternalTypeError:
		return "InternalTypeError";
	case RuntimeErrorCode::InternalFFITypeError:
		return "InternalFFITypeError";
	case RuntimeErrorCode::UnsupportedPlatformOperation:
		return "UnsupportedPlatformOperation";
	case RuntimeErrorCode::WorkerCancelled:
		return "WorkerCancelled";
	case RuntimeErrorCode::WorkerExited:
		return "WorkerExited";
	case RuntimeErrorCode::InvalidWorkerHandle:
		return "InvalidWorkerHandle";
	case RuntimeErrorCode::InvalidWorkerTransfer:
		return "InvalidWorkerTransfer";
	case RuntimeErrorCode::InvalidConversion:
		return "InvalidConversion";
	case RuntimeErrorCode::OutOfMemory:
		return "OutOfMemory";
	default:
		return "None";
	}
}

namespace
{
	std::string NormalizeDiagnosticPath(std::string_view path)
	{
		std::string normalized(path);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		return normalized;
	}

	bool IsRuntimePanicCode(RuntimeErrorCode code)
	{
		switch (code)
		{
		case RuntimeErrorCode::StackOverflow:
		case RuntimeErrorCode::MemoryAccessViolation:
		case RuntimeErrorCode::InternalTypeError:
		case RuntimeErrorCode::InternalFFITypeError:
		case RuntimeErrorCode::OutOfMemory:
			return true;
		case RuntimeErrorCode::None:
		case RuntimeErrorCode::IndexOutOfBounds:
		case RuntimeErrorCode::FFIFunctionNotFound:
		case RuntimeErrorCode::DivisionByZero:
		case RuntimeErrorCode::WorkerCancelled:
		case RuntimeErrorCode::InvalidWorkerHandle:
		case RuntimeErrorCode::InvalidWorkerTransfer:
		case RuntimeErrorCode::InvalidConversion:
		default:
			return false;
		}
	}

	MarmotJson::Document SerializeRuntimeStack(const std::vector<RuntimeStackFrame>& stack)
	{
		MarmotJson::Document serialized = MarmotJson::Document::Array();
		for (size_t index = 0u; index < stack.size(); index += 1u)
		{
			const RuntimeStackFrame& frame = stack[index];
			MarmotJson::Document object;
			std::optional<std::string> normalized_file_storage = std::nullopt;
			std::optional<std::string_view> file = std::nullopt;
			if (!frame.m_location.m_file_name.empty())
			{
				normalized_file_storage = NormalizeDiagnosticPath(frame.m_location.m_file_name);
				file = *normalized_file_storage;
			}

			std::optional<int> line = frame.m_location.m_line > 0 ? std::optional<int>(frame.m_location.m_line) : std::nullopt;
			std::optional<int> end_line = frame.m_location.m_end_line.has_value() ? frame.m_location.m_end_line : line;
			std::optional<int> end_column = frame.m_location.m_end_column;
			if (!end_column.has_value() && frame.m_location.m_column.has_value())
			{
				end_column = *frame.m_location.m_column + static_cast<int>(std::max(frame.m_location.m_caret_length.value_or(size_t(1u)), size_t(1u)));
			}
			const std::optional<std::string_view> source_line =
				frame.m_location.m_source_line.has_value()
					? std::optional<std::string_view>(*frame.m_location.m_source_line)
					: std::nullopt;

			object.AddStringField("procedure", frame.m_procedure_name);
			object.AddStringField("module", frame.m_module_name.empty() ? std::optional<std::string_view>(std::nullopt) : std::optional<std::string_view>(frame.m_module_name));
			object.AddStringField("file", file);
			object.AddNumberField("line", line);
			object.AddNumberField("column", frame.m_location.m_column);
			object.AddNumberField("endLine", end_line);
			object.AddNumberField("endColumn", end_column);
			object.AddStringField("sourceLine", source_line);
			object.AddNumberField("recursiveCount", frame.m_recursive_call_count);

			serialized.Append(object);
		}

		return serialized;
	}

	std::string SerializeMachineReadableRuntimeDiagnostic(const RuntimeError& error)
	{
		MarmotJson::Document serialized;

		std::optional<std::string_view> file = std::nullopt;
		std::optional<std::string> normalized_file_storage = std::nullopt;
		std::optional<int> line = std::nullopt;
		std::optional<int> column = std::nullopt;
		std::optional<size_t> caret_length = std::nullopt;
		std::optional<int> end_line = std::nullopt;
		std::optional<int> end_column = std::nullopt;
		std::optional<std::string_view> source_line = std::nullopt;

		if (error.m_location.has_value())
		{
			normalized_file_storage = NormalizeDiagnosticPath(error.m_location->m_file_name);
			file = normalized_file_storage->empty() ? std::nullopt : std::optional<std::string_view>(*normalized_file_storage);
			if (error.m_location->m_line > 0)
			{
				line = error.m_location->m_line;
			}
			column = error.m_location->m_column;
			caret_length = error.m_location->m_caret_length;
			end_line = error.m_location->m_end_line.has_value() ? error.m_location->m_end_line : line;
			end_column = error.m_location->m_end_column;
			if (!end_column.has_value() && column.has_value())
			{
				end_column = *column + static_cast<int>(std::max(caret_length.value_or(size_t(1u)), size_t(1u)));
			}
			if (error.m_location->m_source_line.has_value())
			{
				source_line = *error.m_location->m_source_line;
			}
		}

		serialized.AddStringField("source", "marmot-runtime");
		serialized.AddStringField("severity", "error");
		serialized.AddStringField("stage", "Runtime");
		serialized.AddStringField("code", RuntimeErrorCodeName(error.m_code));
		serialized.AddStringField("kind", error.m_kind == RuntimeDiagnosticKind::Panic ? "panic" : "error");
		serialized.AddStringField("message", error.m_message);
		serialized.AddStringField("file", file);
		serialized.AddNumberField("line", line);
		serialized.AddNumberField("column", column);
		serialized.AddNumberField("endLine", end_line);
		serialized.AddNumberField("endColumn", end_column);
		serialized.AddNumberField("caret_length", caret_length);
		serialized.AddStringField("sourceLine", source_line);
		serialized.AddNumberField("exitCode", error.ExitCode());
		serialized.AddField("stack", SerializeRuntimeStack(error.m_stack));

		return serialized.Write();
	}

	std::string RenderRuntimeDiagnostic(const RuntimeError& error)
	{
		std::ostringstream oss;
		const bool is_panic = error.m_kind == RuntimeDiagnosticKind::Panic;

		oss << MarmotTerminal::Code(MarmotTerminal::Color::BOLD);
		oss << MarmotTerminal::Code(MarmotTerminal::Color::BRIGHT_RED);
		oss << (is_panic ? "panic" : "error");
		if (error.m_code != RuntimeErrorCode::None)
		{
			oss << "[" << RuntimeErrorCodeName(error.m_code) << "]";
		}
		oss << MarmotTerminal::Code(MarmotTerminal::Color::RESET);
		oss << ": ";
		oss << MarmotTerminal::Code(MarmotTerminal::Color::BRIGHT_WHITE);
		oss << error.m_message;
		oss << MarmotTerminal::Code(MarmotTerminal::Color::RESET) << "\n";

		if (error.m_location.has_value())
		{
			const RuntimeErrorLocation& location = *error.m_location;
			const bool has_line = location.m_line > 0;
			const bool has_file = !location.m_file_name.empty();

			if (has_file || has_line)
			{
				oss << " ";
				oss << MarmotTerminal::Code(MarmotTerminal::Color::BRIGHT_CYAN);
				oss << "-->";
				oss << MarmotTerminal::Code(MarmotTerminal::Color::RESET);
				oss << " ";
				if (has_file)
				{
					oss << location.m_file_name;
				}
				if (has_line)
				{
					if (has_file)
					{
						oss << ":";
					}
					oss << location.m_line;
					if (location.m_column.has_value())
					{
						oss << ":" << (*location.m_column + 1);
					}
				}
				oss << "\n";
			}

			if (location.m_source_line.has_value() && has_line)
			{
				const std::string_view source_text = *location.m_source_line;
				const std::string line_number = std::to_string(location.m_line);
				const int gutter_width = static_cast<int>(line_number.size()) + 1;

				oss << MarmotTerminal::Code(MarmotTerminal::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|" << MarmotTerminal::Code(MarmotTerminal::Color::RESET) << "\n";

				oss << line_number;
				oss << " | ";
				oss << source_text;
				oss << "\n";

				oss << MarmotTerminal::Code(MarmotTerminal::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|";
				oss << MarmotTerminal::Code(MarmotTerminal::Color::BRIGHT_RED);
				if (location.m_column.has_value())
				{
					oss << " ";
					for (int index = 0; index < *location.m_column; index += 1)
					{
						oss << " ";
					}
					for (size_t index = 0u; index < location.m_caret_length.value_or(1u); index += 1u)
					{
						oss << "^";
					}
					oss << " " << error.m_message;
				}
				else
				{
					oss << " " << error.m_message;
				}
				oss << MarmotTerminal::Code(MarmotTerminal::Color::RESET) << "\n";

				oss << MarmotTerminal::Code(MarmotTerminal::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|" << MarmotTerminal::Code(MarmotTerminal::Color::RESET) << "\n";
			}
		}

		if (!error.m_stack.empty())
		{
			oss << "stack trace:\n";
			for (const RuntimeStackFrame& frame : error.m_stack)
			{
				oss << "  at ";
				oss << MarmotTerminal::Code(MarmotTerminal::Color::BRIGHT_YELLOW);
				oss << frame.m_procedure_name;
				oss << MarmotTerminal::Code(MarmotTerminal::Color::RESET);

				if (!frame.m_module_name.empty())
				{
					oss << " [module " << frame.m_module_name << "]";
				}

				if (!frame.m_location.m_file_name.empty())
				{
					oss << " in " << frame.m_location.m_file_name;
				}
				if (frame.m_location.m_line > 0)
				{
					oss << ":" << frame.m_location.m_line;
					if (frame.m_location.m_column.has_value())
					{
						oss << ":" << (*frame.m_location.m_column + 1);
					}
				}
				if (frame.m_recursive_call_count > 1)
				{
					oss << " [x" << frame.m_recursive_call_count << " recursive calls]";
				}
				oss << "\n";

				if (frame.m_location.m_source_line.has_value())
				{
					oss << "    " << *frame.m_location.m_source_line << "\n";
				}
			}
		}

		return oss.str();
	}
}

std::string SerializeMachineReadableRuntimeError(const RuntimeError& error)
{
	return SerializeMachineReadableRuntimeDiagnostic(error);
}

int RuntimeError::ExitCode() const
{
	return m_kind == RuntimeDiagnosticKind::Panic ? 2 : 1;
}

std::string_view RuntimeError::Rendered() const
{
	return m_rendered.empty() ? std::string_view(m_message) : std::string_view(m_rendered);
}

RuntimeError::RuntimeError(RuntimeErrorCode code, std::string_view message, std::optional<RuntimeErrorLocation> location, std::vector<RuntimeStackFrame>&& stack, std::optional<RuntimeDiagnosticKind> kind)
	: m_kind(kind.value_or(IsRuntimePanicCode(code) ? RuntimeDiagnosticKind::Panic : RuntimeDiagnosticKind::Error)),
	m_code(code),
	m_message(message),
	m_location(std::move(location)),
	m_stack(std::move(stack))
{
	if (m_location.has_value())
	{
		if (m_location->m_line > 0 && !m_location->m_end_line.has_value())
		{
			m_location->m_end_line = m_location->m_line;
		}
		if (m_location->m_column.has_value() && !m_location->m_end_column.has_value())
		{
			m_location->m_end_column =
				*m_location->m_column
				+ static_cast<int>(std::max(m_location->m_caret_length.value_or(size_t(1u)), size_t(1u)));
		}
	}

	m_rendered = RenderRuntimeDiagnostic(*this);
}

void FatalOutOfMemory(std::string_view context, size_t bytes) noexcept
{
	const RuntimeError runtime_error(RuntimeErrorCode::OutOfMemory, std::format("Out of memory while allocating {} bytes ({}).", bytes, context));
	std::print("{}", runtime_error.Rendered());
	std::exit(runtime_error.ExitCode());
}
