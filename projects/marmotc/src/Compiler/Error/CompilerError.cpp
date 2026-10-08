#include "CompilerError.h"
#include "Compiler/Json/Json.h"
#include "Compiler/Terminal/Terminal.h"
#include <algorithm>
#include <filesystem>
#include <ranges>
#include <sstream>
#include <utility>

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
	case CompilerStage::Lowering:
		return "Lowering";
	case CompilerStage::CodeGenerator:
		return "CodeGenerator";
	case CompilerStage::Module:
		return "Module";
	case CompilerStage::BytecodeLinker:
		return "BytecodeLinker";
	case CompilerStage::Compiler:
		return "Compiler";
	case CompilerStage::Unknown:
		return "Unknown";
	}
	std::unreachable();
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
	case CompilerErrorCode::TypeUnreachableCase:
		return "TypeUnreachableCase";
	case CompilerErrorCode::CodeGeneratorLimitExceeded:
		return "CodeGeneratorLimitExceeded";
	case CompilerErrorCode::LoweringUnresolvedMethodResolution:
		return "LoweringUnresolvedMethodResolution";
	case CompilerErrorCode::LoweringAmbiguousMethodResolution:
		return "LoweringAmbiguousMethodResolution";
	case CompilerErrorCode::LoweringUnsupportedConstruct:
		return "LoweringUnsupportedConstruct";
	case CompilerErrorCode::LoweringUnknownForeignFunction:
		return "LoweringUnknownForeignFunction";
	case CompilerErrorCode::BytecodeLinkerDuplicateExportedSymbol:
		return "BytecodeLinkerDuplicateExportedSymbol";
	case CompilerErrorCode::BytecodeLinkerUnresolvedImport:
		return "BytecodeLinkerUnresolvedImport";
	case CompilerErrorCode::CompilerInternalError:
		return "CompilerInternalError";
	}
	std::unreachable();
}

std::string_view CompilerWarningCodeName(CompilerWarningCode code)
{
	switch (code)
	{
	case CompilerWarningCode::NameShadowing:
		return "NameShadowing";
	case CompilerWarningCode::UnusedLocal:
		return "UnusedLocal";
	case CompilerWarningCode::CellCrossesWorker:
		return "CellCrossesWorker";
	case CompilerWarningCode::IntegerOverflow:
		return "IntegerOverflow";
	case CompilerWarningCode::None:
		return "None";
	}
	std::unreachable();
}

namespace
{
	enum class DiagnosticSeverity
	{
		Error,
		Warning
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
			case CompilerStage::Lowering:
				return "Lowering Error";
			case CompilerStage::CodeGenerator:
				return "Code Generator Error";
			case CompilerStage::Module:
				return "Module Error";
			case CompilerStage::BytecodeLinker:
				return "Bytecode Linker Error";
			case CompilerStage::Compiler:
				return "Compiler Error";
			case CompilerStage::Unknown:
				return "Error";
			}
			std::unreachable();
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
		case CompilerStage::Lowering:
			return "Lowering Warning";
		case CompilerStage::CodeGenerator:
			return "Code Generator Warning";
		case CompilerStage::Module:
			return "Module Warning";
		case CompilerStage::BytecodeLinker:
			return "Bytecode Linker Warning";
		case CompilerStage::Compiler:
			return "Compiler Warning";
		case CompilerStage::Unknown:
			return "Warning";
		}
		std::unreachable();
	}

	CompilerTerminal::Color DiagnosticAccentColor(DiagnosticSeverity severity)
	{
		return severity == DiagnosticSeverity::Error
			? CompilerTerminal::Color::BRIGHT_RED
			: CompilerTerminal::Color::BRIGHT_YELLOW;
	}

	std::string RenderLabeledLine(CompilerTerminal::Color label_color, CompilerTerminal::Color message_color, std::string_view label, std::string_view message)
	{
		std::ostringstream oss;
		oss << CompilerTerminal::Code(label_color);
		oss << "[" << label << "] ";
		oss << CompilerTerminal::Code(message_color);
		oss << message;
		oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET);
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

			CompilerJson::AppendStringField(object, "message", item.m_message, first_field);
			CompilerJson::AppendStringField(object, "file", file, first_field);
			CompilerJson::AppendStringField(object, "file_path", file, first_field);
			CompilerJson::AppendNumberField(object, "line", line, first_field);
			CompilerJson::AppendNumberField(object, "column", item.m_location.m_column, first_field);
			CompilerJson::AppendNumberField(object, "endLine", end_line, first_field);
			CompilerJson::AppendNumberField(object, "endColumn", end_column, first_field);
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

		CompilerJson::AppendStringField(serialized, "source", "marmot", first_field);
		CompilerJson::AppendStringField(serialized, "severity", severity == DiagnosticSeverity::Error ? "error" : "warning", first_field);
		CompilerJson::AppendStringField(serialized, "stage", CompilerStageName(stage), first_field);
		CompilerJson::AppendStringField(serialized, "code", code_name, first_field);
		CompilerJson::AppendStringField(serialized, "message", message, first_field);
		CompilerJson::AppendStringField(serialized, "file", file, first_field);
		CompilerJson::AppendStringField(serialized, "file_path", file, first_field);
		CompilerJson::AppendNumberField(serialized, "line", line, first_field);
		CompilerJson::AppendNumberField(serialized, "column", column, first_field);
		CompilerJson::AppendNumberField(serialized, "endLine", end_line, first_field);
		CompilerJson::AppendNumberField(serialized, "endColumn", end_column, first_field);
		CompilerJson::AppendNumberField(serialized, "caret_length", caret_length, first_field);
		CompilerJson::AppendStringField(serialized, "suggestion", serialized_suggestion, first_field);
		CompilerJson::AppendRawField(serialized, "relatedInformation", SerializeRelatedInformation(related_information), first_field);
		serialized.push_back('}');
		return serialized;
	}

	// Spaces up to `column`, except a tab where the source line has one and
	// nothing for a UTF-8 continuation byte, so the caret lands under the
	// column whatever the terminal's tab width.
	std::string CaretIndent(std::string_view source_line, int column)
	{
		const size_t width = static_cast<size_t>(column);
		std::string indent = source_line.substr(0u, std::min(width, source_line.size()))
			| std::views::filter([](char c) { return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u; })
			| std::views::transform([](char c) { return c == '\t' ? '\t' : ' '; })
			| std::ranges::to<std::string>();
		indent.append(width - std::min(width, source_line.size()), ' ');
		return indent;
	}

	std::string RenderCompilerDiagnostic(
		CompilerStage stage,
		std::string_view message,
		const std::optional<CompilerErrorLocation>& location,
		const std::optional<std::string>& suggestion,
		DiagnosticSeverity severity)
	{
		std::ostringstream oss;
		oss << CompilerTerminal::Code(CompilerTerminal::Color::BOLD);
		oss << CompilerTerminal::Code(DiagnosticAccentColor(severity));
		oss << StageLabel(stage, severity);
		oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET);
		if (location.has_value())
		{
			oss << " at ";
			oss << CompilerTerminal::Code(CompilerTerminal::Color::BRIGHT_CYAN);
			oss << location->m_file_name;
			if (location->m_line > 0)
			{
				oss << ":" << location->m_line;
			}
			oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET);
		}
		oss << "\n";

		if (location.has_value() && location->m_source_line.has_value() && location->m_line > 0)
		{
			const CompilerErrorLocation& resolved_location = *location;
			std::string_view source_line = *resolved_location.m_source_line;
			const std::string line_num_str = std::to_string(resolved_location.m_line);
			const int gutter_width = static_cast<int>(line_num_str.length()) + 1;

			oss << CompilerTerminal::Code(CompilerTerminal::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|\n";

			oss << line_num_str << " |";
			oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET);
			oss << " " << source_line << "\n";

			oss << CompilerTerminal::Code(CompilerTerminal::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|";
			oss << CompilerTerminal::Code(DiagnosticAccentColor(severity));

			if (resolved_location.m_column.has_value())
			{
				oss << " " << CaretIndent(source_line, *resolved_location.m_column);

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

			oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET) << "\n";

			oss << CompilerTerminal::Code(CompilerTerminal::Color::BLUE);
			for (int i = 0; i < gutter_width; i += 1)
			{
				oss << " ";
			}
			oss << "|" << CompilerTerminal::Code(CompilerTerminal::Color::RESET) << "\n";
		}
		else
		{
			oss << "  " << message << "\n";
		}

		if (suggestion.has_value())
		{
			oss << CompilerTerminal::Code(CompilerTerminal::Color::YELLOW);
			oss << "  | ";
			oss << *suggestion;
			oss << CompilerTerminal::Code(CompilerTerminal::Color::RESET) << "\n";
		}

		return oss.str();
	}

}

std::string RenderWarningGroupHeader(size_t warning_count, std::string_view file_path)
{
	const std::string summary = file_path.empty()
		? std::format("{} warning(s)\n", warning_count)
		: std::format("{} warning(s) in {}\n", warning_count, std::filesystem::path(file_path).filename().string());
	return RenderLabeledLine(CompilerTerminal::Color::YELLOW, CompilerTerminal::Color::WHITE, "warning", summary);
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
	: CompilerError(CompilerStage::Compiler, std::move(message))
{
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

bool CompilerError::IsNoMatch() const
{
	return m_code == CompilerErrorCode::NoMatch;
}

std::string_view CompilerError::Rendered() const
{
	return m_rendered.empty() ? std::string_view(m_message) : std::string_view(m_rendered);
}

CompilerWarning::CompilerWarning(std::string message)
	: CompilerWarning(CompilerStage::Compiler, std::move(message))
{
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

CompilerError MidoriError::GenerateCodeGeneratorErrorWithContext(CompilerErrorCode code, std::string_view message, int line, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	// Use unified implementation without column info (no caret)
	return GenerateRichError(CompilerStage::CodeGenerator, message, line, file_name, source_lines, std::nullopt, std::nullopt, suggestion, code);
}

CompilerError MidoriError::GenerateLoweringErrorWithContext(CompilerErrorCode code, std::string_view message, int line, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::Lowering, message, line, file_name, source_lines, std::nullopt, std::nullopt, suggestion, code);
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
