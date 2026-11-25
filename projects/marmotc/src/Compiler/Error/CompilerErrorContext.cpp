#include "Compiler/Error/CompilerError.h"
#include "Compiler/AbstractSyntaxTree/Type.h"
#include "Compiler/Token/Token.h"
#include "Support/Optional.h"

#include <algorithm>
#include <format>

namespace
{
	struct TokenLocationContext
	{
		MarmotCompiler::Optional<int> m_column = MarmotCompiler::None;
		MarmotCompiler::Optional<size_t> m_caret_length = MarmotCompiler::None;
		MarmotCompiler::Optional<std::string_view> m_source_line = MarmotCompiler::None;
	};

	TokenLocationContext GetTokenLocationContext(const Token& token, const std::vector<std::string>& source_lines)
	{
		TokenLocationContext context;
		if (token.m_line <= 0 || static_cast<size_t>(token.m_line) > source_lines.size())
		{
			return context;
		}

		context.m_source_line = source_lines[static_cast<size_t>(token.m_line - 1)];
		if (!token.m_column.HasValue())
		{
			return context;
		}

		context.m_column = token.m_column;
		context.m_caret_length = std::max(token.m_source_length.ValueOr(size_t(0u)), size_t(1u));
		return context;
	}
}

CompilerError CompilerError::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion, CompilerErrorCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
}

CompilerWarning CompilerWarning::WithToken(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion, CompilerWarningCode code)
{
	const TokenLocationContext context = GetTokenLocationContext(token, source_lines);
	return WithContext(stage, message, token.m_line, file_name, context.m_column, context.m_caret_length, suggestion, context.m_source_line, code);
}

CompilerError MarmotError::GenerateRichError(CompilerStage stage, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion, CompilerErrorCode code)
{
	return CompilerError::WithToken(stage, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MarmotError::GenerateLoweringErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::Lowering, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MarmotError::GenerateParserErrorWithContext(std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion)
{
	return GenerateParserErrorWithContext(CompilerErrorCode::None, message, token, file_name, source_lines, suggestion);
}

CompilerError MarmotError::GenerateParserErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::Parser, message, token, file_name, source_lines, suggestion, code);
}

CompilerError MarmotError::GenerateTypeCheckerErrorWithContext(std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion)
{
	return GenerateTypeCheckerErrorWithContext(CompilerErrorCode::None, message, token, file_name, source_lines, suggestion);
}

CompilerError MarmotError::GenerateTypeCheckerErrorWithContext(CompilerErrorCode code, std::string_view message, const Token& token, std::string_view file_name, const std::vector<std::string>& source_lines, MarmotCompiler::Optional<std::string_view> suggestion)
{
	return GenerateRichError(CompilerStage::TypeChecker, message, token, file_name, source_lines, suggestion, code);
}

std::string MarmotError::FormatTypeMismatch(std::string_view message, const std::vector<std::string>& expected_names, const std::shared_ptr<MarmotType>& actual)
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
