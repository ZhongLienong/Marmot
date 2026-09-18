#pragma once

#include "Compiler/Result/Result.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace MidoriTest
{
	struct WarningExpectation
	{
		std::optional<CompilerStage> m_stage = std::nullopt;
		std::optional<CompilerWarningCode> m_code = std::nullopt;
		std::optional<int> m_line = std::nullopt;
		std::vector<std::string> m_message_substrings;
		std::vector<std::string> m_rendered_substrings;
	};

	struct ErrorExpectation
	{
		std::optional<CompilerStage> m_stage = std::nullopt;
		std::optional<CompilerErrorCode> m_code = std::nullopt;
		std::optional<int> m_line = std::nullopt;
		std::vector<std::string> m_message_substrings;
		std::vector<std::string> m_rendered_substrings;
	};

	[[nodiscard]] std::string StripAnsiCodes(std::string_view text);

	[[nodiscard]] bool Matches(const CompilerWarning& warning, const WarningExpectation& expectation, std::string* mismatch = nullptr);

	[[nodiscard]] bool Matches(const CompilerError& error, const ErrorExpectation& expectation, std::string* mismatch = nullptr);

	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerStage stage, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerStage stage, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage, CompilerWarningCode code);

	[[nodiscard]] const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerStage stage);
	[[nodiscard]] const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerErrorCode code);
	[[nodiscard]] const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerStage stage, CompilerErrorCode code);

	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage);
	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerErrorCode code);
	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage, CompilerErrorCode code);
}
