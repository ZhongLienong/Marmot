#include "Error.h"
#include "Common/Json/Json.h"
#include "Common/Printer/Printer.h"
#include <algorithm>
#include <filesystem>
#include <sstream>

std::string_view CompilerStageName(CompilerStage stage)
{
	switch (stage)
	{
	case CompilerStage::Lexer:
		return "Lexer";
	case CompilerStage::Parser:
		return "Parser";
	case CompilerStage::TypeChecker:
		return "TypeChecker";
	case CompilerStage::StaticAnalyzer:
		return "StaticAnalyzer";
	case CompilerStage::CodeGenerator:
		return "CodeGenerator";
	case CompilerStage::Module:
		return "Module";
	case CompilerStage::Optimizer:
		return "Optimizer";
	case CompilerStage::BytecodeLinker:
		return "BytecodeLinker";
	case CompilerStage::Compiler:
		return "Compiler";
	case CompilerStage::Runtime:
		return "Runtime";
	case CompilerStage::Unknown:
	default:
		return "Unknown";
	}
}

std::string_view CompilerErrorCodeName(CompilerErrorCode code)
{
	switch (code)
	{
	case CompilerErrorCode::None:
		return "None";
	case CompilerErrorCode::NoMatch:
		return "NoMatch";
	case CompilerErrorCode::ModuleImportResolutionFailed:
		return "ModuleImportResolutionFailed";
	case CompilerErrorCode::ModuleImportFileOpenFailed:
		return "ModuleImportFileOpenFailed";
	case CompilerErrorCode::ModuleCircularDependency:
		return "ModuleCircularDependency";
	case CompilerErrorCode::ModuleDeclarationMissing:
		return "ModuleDeclarationMissing";
	case CompilerErrorCode::ModuleDeclarationDuplicate:
		return "ModuleDeclarationDuplicate";
	case CompilerErrorCode::ModuleMissingExportedSymbol:
		return "ModuleMissingExportedSymbol";
	case CompilerErrorCode::TypeUndefinedName:
		return "TypeUndefinedName";
	case CompilerErrorCode::TypeUnsatisfiedConstraint:
		return "TypeUnsatisfiedConstraint";
	case CompilerErrorCode::TypeNotCallable:
		return "TypeNotCallable";
	case CompilerErrorCode::TypeIncorrectArity:
		return "TypeIncorrectArity";
	case CompilerErrorCode::TypeMismatch:
		return "TypeMismatch";
	case CompilerErrorCode::TypeNonExhaustiveMatch:
		return "TypeNonExhaustiveMatch";
	case CompilerErrorCode::CodeGeneratorLimitExceeded:
		return "CodeGeneratorLimitExceeded";
	case CompilerErrorCode::CodeGeneratorUnresolvedMethodResolution:
		return "CodeGeneratorUnresolvedMethodResolution";
	case CompilerErrorCode::CodeGeneratorAmbiguousMethodResolution:
		return "CodeGeneratorAmbiguousMethodResolution";
	case CompilerErrorCode::CodeGeneratorUnsupportedLowering:
		return "CodeGeneratorUnsupportedLowering";
	case CompilerErrorCode::CodeGeneratorUnknownForeignFunction:
		return "CodeGeneratorUnknownForeignFunction";
	case CompilerErrorCode::BytecodeLinkerNoModulesToLink:
		return "BytecodeLinkerNoModulesToLink";
	case CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol:
		return "BytecodeLinkerDuplicateExportedSymbol";
	case CompilerErrorCode::BytecodeLinkerUnresolvedImport:
		return "BytecodeLinkerUnresolvedImport";
	case CompilerErrorCode::CompilerNoModulesReadyToCompile:
		return "CompilerNoModulesReadyToCompile";
	case CompilerErrorCode::CompilerIncompleteCompilationSchedule:
		return "CompilerIncompleteCompilationSchedule";
	case CompilerErrorCode::CompilerMissingCompiledModule:
		return "CompilerMissingCompiledModule";
	default:
		return "None";
	}
}

std::string_view CompilerWarningCodeName(CompilerWarningCode code)
{
	switch (code)
	{
	case CompilerWarningCode::NameShadowing:
		return "NameShadowing";
	case CompilerWarningCode::UnusedLocal:
		return "UnusedLocal";
	case CompilerWarningCode::CaptureEscape:
		return "CaptureEscape";
	case CompilerWarningCode::CellCrossesWorker:
		return "CellCrossesWorker";
	case CompilerWarningCode::IntegerOverflow:
		return "IntegerOverflow";
	case CompilerWarningCode::None:
	default:
		return "None";
	}
}

std::string_view RuntimeErrorCodeName(RuntimeErrorCode code)
{
	switch (code)
	{
	case RuntimeErrorCode::None:
		return "None";
	case RuntimeErrorCode::IndexOutOfBounds:
		return "IndexOutOfBounds";
	case RuntimeErrorCode::NegativeArraySize:
		return "NegativeArraySize";
	case RuntimeErrorCode::ArraySizeExceeded:
		return "ArraySizeExceeded";
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
	default:
		return "None";
	}
}

namespace
{
	enum class DiagnosticSeverity
	{
		Error,
		Warning
	};

	struct TokenLocationContext
	{
		std::optional<int> m_column = std::nullopt;
		std::optional<size_t> m_caret_length = std::nullopt;
		std::optional<std::string_view> m_source_line = std::nullopt;
	};

	std::string NormalizeDiagnosticPath(std::string_view path)
	{
		std::string normalized(path);
		std::replace(normalized.begin(), normalized.end(), '\\', '/');
		return normalized;
	}

	std::string_view StageLabel(CompilerStage stage, DiagnosticSeverity severity)
	{
		if (severity == DiagnosticSeverity::Error)
		{
			switch (stage)
			{
			case CompilerStage::Lexer:
				return "Lexer Error";
			case CompilerStage::Parser:
				return "Parser Error";
			case CompilerStage::TypeChecker:
				return "Type Checker Error";
			case CompilerStage::StaticAnalyzer:
				return "Static Analyzer Error";
			case CompilerStage::CodeGenerator:
				return "Code Generator Error";
			case CompilerStage::Module:
				return "Module Error";
			case CompilerStage::Optimizer:
				return "Optimizer Error";
			case CompilerStage::BytecodeLinker:
				return "Bytecode Linker Error";
			case CompilerStage::Compiler:
				return "Compiler Error";
			case CompilerStage::Runtime:
				return "Runtime Error";
			case CompilerStage::Unknown:
			default:
				return "Error";
			}
		}

		switch (stage)
		{
		case CompilerStage::Lexer:
			return "Lexer Warning";
		case CompilerStage::Parser:
			return "Parser Warning";
		case CompilerStage::TypeChecker:
			return "Type Checker Warning";
		case CompilerStage::StaticAnalyzer:
			return "Static Analyzer Warning";
		case CompilerStage::CodeGenerator:
			return "Code Generator Warning";
		case CompilerStage::Module:
			return "Module Warning";
		case CompilerStage::Optimizer:
			return "Optimizer Warning";
		case CompilerStage::BytecodeLinker:
			return "Bytecode Linker Warning";
		case CompilerStage::Compiler:
			return "Compiler Warning";
		case CompilerStage::Runtime:
			return "Runtime Warning";
		case CompilerStage::Unknown:
		default:
			return "Warning";
		}
	}

	Printer::Color DiagnosticAccentColor(DiagnosticSeverity severity)
	{
		return severity == DiagnosticSeverity::Error
			? Printer::Color::BRIGHT_RED
			: Printer::Color::BRIGHT_YELLOW;
	}

	std::string RenderLabeledLine(Printer::Color label_color, Printer::Color message_color, std::string_view label, std::string_view message)
	{
		std::ostringstream oss;
		oss << Printer::Detail::GetColorCode(label_color);
		oss << "[" << label << "] ";
		oss << Printer::Detail::GetColorCode(message_color);
		oss << message;
		oss << "\033[0m";
		return oss.str();
	}

	std::string SerializeRelatedInformation(const std::vector<CompilerRelatedInformation>& related_information)
	{
		std::string serialized = "[";
		for (size_t index = 0u; index < related_information.size(); index += 1u)
		{
			if (index > 0u)
			{
				serialized.push_back(',');
			}

			const CompilerRelatedInformation& item = related_information[index];
			std::string object = "{";
			bool first_field = true;
			const std::optional<std::string> normalized_file =
				item.m_location.m_file_name.empty() ? std::nullopt : std::optional<std::string>(NormalizeDiagnosticPath(item.m_location.m_file_name));
			const std::optional<std::string_view> file =
				normalized_file.has_value() ? std::optional<std::string_view>(*normalized_file) : std::nullopt;
			std::optional<int> line = item.m_location.m_line > 0 ? std::optional<int>(item.m_location.m_line) : std::nullopt;
			std::optional<int> end_line = item.m_location.m_end_line.has_value()
				? item.m_location.m_end_line
				: line;
			std::optional<int> end_column = item.m_location.m_end_column;
			if (!end_column.has_value() && item.m_location.m_column.has_value())
			{
				end_column = *item.m_location.m_column + static_cast<int>(std::max(item.m_location.m_caret_length.value_or(size_t(1u)), size_t(1u)));
			}

			MidoriJson::AppendStringField(object, "message", item.m_message, first_field);
			MidoriJson::AppendStringField(object, "file", file, first_field);
			MidoriJson::AppendStringField(object, "file_path", file, first_field);
			MidoriJson::AppendNumberField(object, "line", line, first_field);
			MidoriJson::AppendNumberField(object, "column", item.m_location.m_column, first_field);
			MidoriJson::AppendNumberField(object, "endLine", end_line, first_field);
			MidoriJson::AppendNumberField(object, "endColumn", end_column, first_field);
			object.push_back('}');
			serialized += object;
		}

		serialized.push_back(']');
		return serialized;
	}

	std::string SerializeMachineReadableDiagnostic(
		DiagnosticSeverity severity,
		CompilerStage stage,
		std::string_view code_name,
		const std::optional<CompilerErrorLocation>& location,
		std::string_view message,
		const std::optional<std::string>& suggestion,
		const std::vector<CompilerRelatedInformation>& related_information)
	{
		std::string serialized = "{";
		bool first_field = true;

		std::optional<std::string_view> file = std::nullopt;
		std::optional<std::string> normalized_file_storage = std::nullopt;
		std::optional<int> line = std::nullopt;
		std::optional<int> column = std::nullopt;
		std::optional<size_t> caret_length = std::nullopt;
		std::optional<int> end_line = std::nullopt;
		std::optional<int> end_column = std::nullopt;
		if (location.has_value())
		{
			normalized_file_storage = NormalizeDiagnosticPath(location->m_file_name);
			file = normalized_file_storage->empty() ? std::nullopt : std::optional<std::string_view>(*normalized_file_storage);
			if (location->m_line > 0)
			{
				line = location->m_line;
			}

			column = location->m_column;
			caret_length = location->m_caret_length;
			end_line = location->m_end_line.has_value() ? location->m_end_line : line;
			end_column = location->m_end_column;
			if (!end_column.has_value() && column.has_value())
			{
				end_column = *column + static_cast<int>(std::max(caret_length.value_or(size_t(1u)), size_t(1u)));
			}
		}

		const std::optional<std::string_view> serialized_suggestion =
			suggestion.has_value() ? std::optional<std::string_view>(*suggestion) : std::optional<std::string_view>(std::nullopt);

		MidoriJson::AppendStringField(serialized, "source", "marmot", first_field);
		MidoriJson::AppendStringField(serialized, "severity", severity == DiagnosticSeverity::Error ? "error" : "warning", first_field);
		MidoriJson::AppendStringField(serialized, "stage", CompilerStageName(stage), first_field);
		MidoriJson::AppendStringField(serialized, "code", code_name, first_field);
		MidoriJson::AppendStringField(serialized, "message", message, first_field);
		MidoriJson::AppendStringField(serialized, "file", file, first_field);
		MidoriJson::AppendStringField(serialized, "file_path", file, first_field);
		MidoriJson::AppendNumberField(serialized, "line", line, first_field);
		MidoriJson::AppendNumberField(serialized, "column", column, first_field);
		MidoriJson::AppendNumberField(serialized, "endLine", end_line, first_field);
		MidoriJson::AppendNumberField(serialized, "endColumn", end_column, first_field);
		MidoriJson::AppendNumberField(serialized, "caret_length", caret_length, first_field);
		MidoriJson::AppendStringField(serialized, "suggestion", serialized_suggestion, first_field);
		MidoriJson::AppendRawField(serialized, "relatedInformation", SerializeRelatedInformation(related_information), first_field);
		serialized.push_back('}');
		return serialized;
	}

	bool IsRuntimePanicCode(RuntimeErrorCode code)
	{
		switch (code)
		{
		case RuntimeErrorCode::StackOverflow:
		case RuntimeErrorCode::MemoryAccessViolation:
		case RuntimeErrorCode::InternalTypeError:
		case RuntimeErrorCode::InternalFFITypeError:
			return true;
		case RuntimeErrorCode::None:
		case RuntimeErrorCode::IndexOutOfBounds:
		case RuntimeErrorCode::NegativeArraySize:
		case RuntimeErrorCode::ArraySizeExceeded:
		case RuntimeErrorCode::FFIFunctionNotFound:
		case RuntimeErrorCode::DivisionByZero:
		case RuntimeErrorCode::WorkerCancelled:
		default:
			return false;
		}
	}

	std::string SerializeRuntimeStack(const std::vector<RuntimeStackFrame>& stack)
	{
		std::string serialized = "[";
		for (size_t index = 0u; index < stack.size(); index += 1u)
		{
			if (index > 0u)
			{
				serialized.push_back(',');
			}

			const RuntimeStackFrame& frame = stack[index];
			std::string object = "{";
			bool first_field = true;
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

			MidoriJson::AppendStringField(object, "procedure", frame.m_procedure_name, first_field);
			MidoriJson::AppendStringField(object, "module", frame.m_module_name.empty() ? std::optional<std::string_view>(std::nullopt) : std::optional<std::string_view>(frame.m_module_name), first_field);
			MidoriJson::AppendStringField(object, "file", file, first_field);
			MidoriJson::AppendStringField(object, "file_path", file, first_field);
			MidoriJson::AppendNumberField(object, "line", line, first_field);
			MidoriJson::AppendNumberField(object, "column", frame.m_location.m_column, first_field);
			MidoriJson::AppendNumberField(object, "endLine", end_line, first_field);
			MidoriJson::AppendNumberField(object, "endColumn", end_column, first_field);
			MidoriJson::AppendStringField(object, "sourceLine", source_line, first_field);
			MidoriJson::AppendNumberField(object, "recursiveCount", frame.m_recursive_call_count, first_field);
			object.push_back('}');
			serialized += object;
		}

		serialized.push_back(']');
		return serialized;
	}

	std::string SerializeMachineReadableRuntimeDiagnostic(const RuntimeError& error)
	{
		std::string serialized = "{";
		bool first_field = true;

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

		MidoriJson::AppendStringField(serialized, "source", "marmot-runtime", first_field);
		MidoriJson::AppendStringField(serialized, "severity", "error", first_field);
		MidoriJson::AppendStringField(serialized, "stage", CompilerStageName(CompilerStage::Runtime), first_field);
		MidoriJson::AppendStringField(serialized, "code", RuntimeErrorCodeName(error.m_code), first_field);
		MidoriJson::AppendStringField(serialized, "kind", error.m_kind == RuntimeDiagnosticKind::Panic ? "panic" : "error", first_field);
		MidoriJson::AppendStringField(serialized, "message", error.m_message, first_field);
		MidoriJson::AppendStringField(serialized, "file", file, first_field);
		MidoriJson::AppendStringField(serialized, "file_path", file, first_field);
		MidoriJson::AppendNumberField(serialized, "line", line, first_field);
		MidoriJson::AppendNumberField(serialized, "column", column, first_field);
		MidoriJson::AppendNumberField(serialized, "endLine", end_line, first_field);
		MidoriJson::AppendNumberField(serialized, "endColumn", end_column, first_field);
		MidoriJson::AppendNumberField(serialized, "caret_length", caret_length, first_field);
		MidoriJson::AppendStringField(serialized, "sourceLine", source_line, first_field);
		MidoriJson::AppendNumberField(serialized, "exitCode", error.ExitCode(), first_field);
		MidoriJson::AppendRawField(serialized, "stack", SerializeRuntimeStack(error.m_stack), first_field);
		serialized.push_back('}');
		return serialized;
	}

	std::string RenderRuntimeDiagnostic(const RuntimeError& error)
	{
		std::ostringstream oss;
		const bool is_panic = error.m_kind == RuntimeDiagnosticKind::Panic;

		oss << Printer::Detail::GetStyleCode(Printer::Style::BOLD);
		oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_RED);
		oss << (is_panic ? "panic" : "error");
		if (error.m_code != RuntimeErrorCode::None)
		{
			oss << "[" << RuntimeErrorCodeName(error.m_code) << "]";
		}
		oss << "\033[0m";
		oss << ": ";
		oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_WHITE);
		oss << error.m_message;
		oss << "\033[0m\n";

		if (error.m_location.has_value())
		{
			const CompilerErrorLocation& location = *error.m_location;
			const bool has_line = location.m_line > 0;
			const bool has_file = !location.m_file_name.empty();

			if (has_file || has_line)
			{
				oss << " ";
				oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_CYAN);
				oss << "-->";
				oss << "\033[0m";
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

				oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|\033[0m\n";

				oss << line_number;
				oss << " | ";
				oss << source_text;
				oss << "\n";

				oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|";
				oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_RED);
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
				oss << "\033[0m\n";

				oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
				for (int index = 0; index < gutter_width; index += 1)
				{
					oss << " ";
				}
				oss << "|\033[0m\n";
			}
		}

		if (!error.m_stack.empty())
		{
			oss << "stack trace:\n";
			for (const RuntimeStackFrame& frame : error.m_stack)
			{
				oss << "  at ";
				oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_YELLOW);
				oss << frame.m_procedure_name;
				oss << "\033[0m";

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

	std::string RenderCompilerDiagnostic(
		CompilerStage stage,
		std::string_view message,
		const std::optional<CompilerErrorLocation>& location,
		const std::optional<std::string>& suggestion,
		DiagnosticSeverity severity)
	{
		if (!location.has_value())
		{
			return std::string(message);
		}

		const CompilerErrorLocation& resolved_location = *location;
		std::ostringstream oss;

		oss << Printer::Detail::GetStyleCode(Printer::Style::BOLD);
		oss << Printer::Detail::GetColorCode(DiagnosticAccentColor(severity));
		oss << StageLabel(stage, severity);
		oss << "\033[0m";
		oss << " at ";
		oss << Printer::Detail::GetColorCode(Printer::Color::BRIGHT_CYAN);
		oss << resolved_location.m_file_name << ":" << resolved_location.m_line;
		oss << "\033[0m\n";

		if (resolved_location.m_source_line.has_value() && resolved_location.m_line > 0)
		{
			std::string_view source_line = *resolved_location.m_source_line;
			const std::string line_num_str = std::to_string(resolved_location.m_line);
			const int gutter_width = static_cast<int>(line_num_str.length()) + 1;

			oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|\n";

			oss << line_num_str << " |";
			oss << "\033[0m";
			oss << " " << source_line << "\n";

			oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|";
			oss << Printer::Detail::GetColorCode(DiagnosticAccentColor(severity));

			if (resolved_location.m_column.has_value())
			{
				oss << " ";
				for (int i = 0; i < *resolved_location.m_column; i += 1)
				{
					oss << " ";
				}

				const size_t caret_length = resolved_location.m_caret_length.value_or(1u);
				for (size_t i = 0u; i < caret_length; i += 1u)
				{
					oss << "^";
				}

				oss << " " << message;
			}
			else
			{
				oss << " " << message;
			}

			oss << "\033[0m\n";

			oss << Printer::Detail::GetColorCode(Printer::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|\033[0m\n";
		}
		else
		{
			oss << "  " << message << "\n";
		}

		if (suggestion.has_value())
		{
			oss << Printer::Detail::GetColorCode(Printer::Color::YELLOW);
			oss << "  | ";
			oss << *suggestion;
			oss << "\033[0m\n";
		}

		return oss.str();
	}

	TokenLocationContext GetTokenLocationContext(const Token& token, const std::vector<std::string>& source_lines)
	{
		TokenLocationContext context;
		if (token.m_line <= 0 || static_cast<size_t>(token.m_line) > source_lines.size())
		{
			return context;
		}

		context.m_source_line = source_lines[static_cast<size_t>(token.m_line - 1)];
		if (!token.m_column.has_value())
		{
			return context;
		}

		context.m_column = token.m_column;
		context.m_caret_length = std::max(token.m_source_length.value_or(size_t(0u)), size_t(1u));
		return context;
	}
}

std::string RenderWarningGroupHeader(size_t warning_count, std::string_view file_path)
{
	const std::string summary = file_path.empty()
		? std::format("{} warning(s)\n", warning_count)
		: std::format("{} warning(s) in {}\n", warning_count, std::filesystem::path(file_path).filename().string());
	return RenderLabeledLine(Printer::Color::YELLOW, Printer::Color::WHITE, "warning", summary);
}

std::string SerializeMachineReadableError(const CompilerError& error)
{
	return SerializeMachineReadableDiagnostic(
		DiagnosticSeverity::Error,
		error.m_stage,
		CompilerErrorCodeName(error.m_code),
		error.m_location,
		error.m_message,
		error.m_suggestion,
		error.m_related_information);
}

std::string SerializeMachineReadableWarningPayload(const CompilerWarning& warning)
{
	return SerializeMachineReadableDiagnostic(
		DiagnosticSeverity::Warning,
		warning.m_stage,
		CompilerWarningCodeName(warning.m_code),
		warning.m_location,
		warning.m_message,
		warning.m_suggestion,
		warning.m_related_information);
}

std::string SerializeMachineReadableWarning(const CompilerWarning& warning)
{
	return "MARMOT_WARNING\t" + SerializeMachineReadableWarningPayload(warning);
}

std::string SerializeMachineReadableRuntimeError(const RuntimeError& error)
{
	return SerializeMachineReadableRuntimeDiagnostic(error);
}

namespace
{
	std::string RenderCompilerError(const CompilerError& error)
	{
		return RenderCompilerDiagnostic(error.m_stage, error.m_message, error.m_location, error.m_suggestion, DiagnosticSeverity::Error);
	}

	std::string RenderCompilerWarning(const CompilerWarning& warning)
	{
		return RenderCompilerDiagnostic(warning.m_stage, warning.m_message, warning.m_location, warning.m_suggestion, DiagnosticSeverity::Warning);
	}
}

CompilerError::CompilerError(std::string message)
	: m_stage(CompilerStage::Compiler),
	m_message(std::move(message))
{
	m_rendered = m_message;
}

CompilerError::CompilerError(std::string_view message)
	: CompilerError(std::string(message))
{
}

CompilerError::CompilerError(const char* message)
	: CompilerError(std::string(message))
{
}

CompilerError::CompilerError(CompilerStage stage, std::string message)
	: m_stage(stage),
	m_message(std::move(message))
{
	m_rendered = RenderCompilerError(*this);
}

CompilerError CompilerError::NoMatch()
{
	CompilerError error;
	error.m_stage = CompilerStage::Parser;
	error.m_code = CompilerErrorCode::NoMatch;
	return error;
}

CompilerError CompilerError::Simple(CompilerStage stage, std::string_view message, CompilerErrorCode code)
{
	CompilerError error(stage, std::string(message));
	error.m_code = code;
	return error;
}

CompilerError CompilerError::WithFile(CompilerStage stage, std::string_view message, std::string_view file_name, CompilerErrorCode code)
{
	if (file_name.empty())
	{
		return Simple(stage, message, code);
	}

	CompilerError error;
	error.m_stage = stage;
	error.m_code = code;
	error.m_message = std::string(message);

	CompilerErrorLocation location;
	location.m_file_name = std::string(file_name);
	error.m_location = std::move(location);

	error.m_rendered = RenderCompilerError(error);
	return error;
}

CompilerError CompilerError::WithContext(CompilerStage stage, std::string_view message, int line, std::string_view file_name, std::optional<int> column, std::optional<size_t> caret_length, std::optional<std::string_view> suggestion, std::optional<std::string_view> source_line, CompilerErrorCode code)
{
	CompilerError error;
	error.m_stage = stage;
	error.m_code = code;
	error.m_message = std::string(message);

	CompilerErrorLocation location;
	location.m_file_name = std::string(file_name);
	location.m_line = line;
	location.m_column = column;
	location.m_caret_length = caret_length;
	if (line > 0)
	{
		location.m_end_line = line;
	}
	if (column.has_value())
	{
		location.m_end_column = *column + static_cast<int>(std::max(caret_length.value_or(size_t(1u)), size_t(1u)));
	}
	if (source_line.has_value())
	{
		location.m_source_line = std::string(*source_line);
	}
	error.m_location = std::move(location);

	if (suggestion.has_value())
	{
		error.m_suggestion = std::string(*suggestion);
	}

	error.m_rendered = RenderCompilerError(error);
	return error;
}

CompilerError CompilerError::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion, CompilerErrorCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
}

bool CompilerError::IsNoMatch() const
{
	return m_code == CompilerErrorCode::NoMatch;
}

std::string_view CompilerError::Rendered() const
{
	return m_rendered.empty() ? std::string_view(m_message) : std::string_view(m_rendered);
}

int RuntimeError::ExitCode() const
{
	return m_kind == RuntimeDiagnosticKind::Panic ? 2 : 1;
}

std::string_view RuntimeError::Rendered() const
{
	return m_rendered.empty() ? std::string_view(m_message) : std::string_view(m_rendered);
}

CompilerError RuntimeError::ToCompilerError() const
{
	std::optional<std::string_view> source_line = std::nullopt;
	std::optional<int> column = std::nullopt;
	std::optional<size_t> caret_length = std::nullopt;
	int line = 0;
	std::string_view file_name;

	if (m_location.has_value())
	{
		line = m_location->m_line;
		file_name = m_location->m_file_name;
		column = m_location->m_column;
		caret_length = m_location->m_caret_length;
		if (m_location->m_source_line.has_value())
		{
			source_line = *m_location->m_source_line;
		}
	}

	CompilerError error = CompilerError::WithContext(
		CompilerStage::Runtime,
		m_message,
		line,
		file_name,
		column,
		caret_length,
		std::nullopt,
		source_line,
		CompilerErrorCode::None);
	error.m_rendered = m_rendered;
	return error;
}

CompilerWarning::CompilerWarning(std::string message)
	: m_stage(CompilerStage::Compiler),
	m_message(std::move(message))
{
	m_rendered = m_message;
}

CompilerWarning::CompilerWarning(std::string_view message)
	: CompilerWarning(std::string(message))
{
}

CompilerWarning::CompilerWarning(const char* message)
	: CompilerWarning(std::string(message))
{
}

CompilerWarning::CompilerWarning(CompilerStage stage, std::string message)
	: m_stage(stage),
	m_message(std::move(message))
{
	m_rendered = RenderCompilerWarning(*this);
}

CompilerWarning CompilerWarning::Simple(CompilerStage stage, std::string_view message, CompilerWarningCode code)
{
	CompilerWarning warning(stage, std::string(message));
	warning.m_code = code;
	return warning;
}

CompilerWarning CompilerWarning::WithContext(CompilerStage stage, std::string_view message, int line, std::string_view file_name, std::optional<int> column, std::optional<size_t> caret_length, std::optional<std::string_view> suggestion, std::optional<std::string_view> source_line, CompilerWarningCode code)
{
	CompilerWarning warning;
	warning.m_stage = stage;
	warning.m_code = code;
	warning.m_message = std::string(message);

	CompilerErrorLocation location;
	location.m_file_name = std::string(file_name);
	location.m_line = line;
	location.m_column = column;
	location.m_caret_length = caret_length;
	if (line > 0)
	{
		location.m_end_line = line;
	}
	if (column.has_value())
	{
		location.m_end_column = *column + static_cast<int>(std::max(caret_length.value_or(size_t(1u)), size_t(1u)));
	}
	if (source_line.has_value())
	{
		location.m_source_line = std::string(*source_line);
	}
	warning.m_location = std::move(location);

	if (suggestion.has_value())
	{
		warning.m_suggestion = std::string(*suggestion);
	}

	warning.m_rendered = RenderCompilerWarning(warning);
	return warning;
}

CompilerWarning CompilerWarning::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion, CompilerWarningCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
}

std::string_view CompilerWarning::Rendered() const
{
	return m_rendered.empty() ? std::string_view(m_message) : std::string_view(m_rendered);
}

CompilerError MidoriError::GenerateRichError(CompilerStage stage, std::string_view message, int line, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<int> column, std::optional<size_t> caret_length, std::optional<std::string_view> suggestion, CompilerErrorCode code)
{
	std::optional<std::string_view> source_line = std::nullopt;
	if (line > 0 && static_cast<size_t>(line) <= source_lines.size())
	{
		source_line = source_lines[line - 1];
	}

	return CompilerError::WithContext(stage, message, line, file_name, column, caret_length, suggestion, source_line, code);
}

CompilerError MidoriError::GenerateRichError(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion, CompilerErrorCode code)
{
	return CompilerError::WithToken(stage, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::CodeGenerator, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MidoriError::GenerateCodeGeneratorErrorWithContext(std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::None, message, token, file_name, source_lines, suggestion);
}

CompilerError MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode code, std::string_view message, int line, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	// Use unified implementation without column info (no caret)
	return GenerateRichError(CompilerStage::CodeGenerator, message, line, file_name, source_lines, std::nullopt, std::nullopt, suggestion, code);
}

CompilerError MidoriError::GenerateCodeGeneratorErrorWithContext(std::string_view message, int line, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateCodeGeneratorErrorWithContext(CompilerErrorCode::None, message, line, file_name, source_lines, suggestion);
}

CompilerError MidoriError::GenerateLexerErrorWithContext(std::string_view message, int line, int column, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	// Use unified implementation with column and single caret
	return GenerateRichError(CompilerStage::Lexer, message, line, file_name, source_lines, column, 1, suggestion);
}

CompilerError MidoriError::GenerateModuleErrorWithContext(std::string_view message, int line, std::string_view file_name, std::optional<std::string_view> suggestion)
{
	return GenerateModuleErrorWithContext(CompilerErrorCode::None, message, line, file_name, suggestion);
}

CompilerError MidoriError::GenerateModuleErrorWithContext(CompilerErrorCode code, std::string_view message, int line, std::string_view file_name, std::optional<std::string_view> suggestion)
{
	return CompilerError::WithContext(CompilerStage::Module, message, line, file_name, std::nullopt, std::nullopt, suggestion, std::nullopt, code);
}

CompilerError MidoriError::GenerateParserErrorWithContext(std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateParserErrorWithContext(CompilerErrorCode::None, message, token, file_name, source_lines, suggestion);
}

CompilerError MidoriError::GenerateParserErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::Parser, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MidoriError::GenerateTypeCheckerErrorWithContext(std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateTypeCheckerErrorWithContext(CompilerErrorCode::None, message, token, file_name, source_lines, suggestion);
}

CompilerError MidoriError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::TypeChecker, message, token, file_name, source_lines, suggestion, code);
}

RuntimeError MidoriError::GenerateRuntimeError(RuntimeErrorCode code, std::string_view message, std::optional<CompilerErrorLocation> location, std::vector<RuntimeStackFrame>&& stack, std::optional<RuntimeDiagnosticKind> kind)
{
	RuntimeError error;
	error.m_code = code;
	error.m_kind = kind.value_or(IsRuntimePanicCode(code) ? RuntimeDiagnosticKind::Panic : RuntimeDiagnosticKind::Error);
	error.m_message = std::string(message);
	error.m_location = std::move(location);
	error.m_stack = std::move(stack);

	if (error.m_location.has_value())
	{
		if (error.m_location->m_line > 0 && !error.m_location->m_end_line.has_value())
		{
			error.m_location->m_end_line = error.m_location->m_line;
		}
		if (error.m_location->m_column.has_value() && !error.m_location->m_end_column.has_value())
		{
			error.m_location->m_end_column =
				*error.m_location->m_column
				+ static_cast<int>(std::max(error.m_location->m_caret_length.value_or(size_t(1u)), size_t(1u)));
		}
	}

	error.m_rendered = RenderRuntimeDiagnostic(error);
	return error;
}
