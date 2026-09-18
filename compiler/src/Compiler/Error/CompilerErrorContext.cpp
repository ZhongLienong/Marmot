#include "Common/Error/Error.h"
#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/Token/Token.h"

#include <algorithm>
#include <format>

// Error.h is shared with the runtime and only forward-declares Token and
// MidoriType. The members that read a token's location or print a type are
// defined here, in the compiler, so the shared library never depends on
// compiler types.

namespace
{
	struct TokenLocationContext
	{
		std::optional<int> m_column = std::nullopt;
		std::optional<size_t> m_caret_length = std::nullopt;
		std::optional<std::string_view> m_source_line = std::nullopt;
	};

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

CompilerError CompilerError::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion, CompilerErrorCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
}

CompilerWarning CompilerWarning::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, std::optional<std::string_view> suggestion, CompilerWarningCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
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

std::string MidoriError::FormatTypeMismatch(std::string_view message, const std::vector<std::string>& expected_names, const std::shared_ptr<MidoriType>& actual)
{
	std::string expected_types;
	for (size_t i = 0u; i < expected_names.size(); i += 1u)
	{
		expected_types.append(expected_names[i]);
		if (i != expected_names.size() - 1u)
		{
			expected_types.append(" or ");
		}
	}

	return std::format("{}\nExpected {}, but got {}", message, expected_types, actual->ToString());
}
