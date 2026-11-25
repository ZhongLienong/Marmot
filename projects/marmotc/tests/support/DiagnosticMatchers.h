#pragma once

#include "Compiler/Result/Result.h"
#include "Support/Optional.h"

#include <string>
#include <string_view>
#include <vector>

namespace MarmotTest
{
	struct WarningExpectation
	{
		MarmotCompiler::Optional<CompilerStage> m_stage = MarmotCompiler::None;
		MarmotCompiler::Optional<CompilerWarningCode> m_code = MarmotCompiler::None;
		MarmotCompiler::Optional<int> m_line = MarmotCompiler::None;
		std::vector<std::string> m_message_substrings;
		std::vector<std::string> m_rendered_substrings;
	};

	struct ErrorExpectation
	{
		MarmotCompiler::Optional<CompilerStage> m_stage = MarmotCompiler::None;
		MarmotCompiler::Optional<CompilerErrorCode> m_code = MarmotCompiler::None;
		MarmotCompiler::Optional<int> m_line = MarmotCompiler::None;
		std::vector<std::string> m_message_substrings;
		std::vector<std::string> m_rendered_substrings;
	};

	[[nodiscard]] std::string StripAnsiCodes(std::string_view text);

	[[nodiscard]] bool Matches(const CompilerWarning& warning, const WarningExpectation& expectation, std::string* mismatch = nullptr);

	[[nodiscard]] bool Matches(const CompilerError& error, const ErrorExpectation& expectation, std::string* mismatch = nullptr);

	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerWarnings& warnings, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerWarnings& warnings, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerWarnings& warnings, CompilerStage stage, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerReport& report, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerReport& report, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const MarmotResult::CompilerReport& report, CompilerStage stage, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerWarningCode code);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage);
	[[nodiscard]] const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage, CompilerWarningCode code);

	[[nodiscard]] const CompilerError* FindError(const MarmotResult::CompilerDiagnostics& diagnostics, CompilerStage stage);
	[[nodiscard]] const CompilerError* FindError(const MarmotResult::CompilerDiagnostics& diagnostics, CompilerErrorCode code);
	[[nodiscard]] const CompilerError* FindError(const MarmotResult::CompilerDiagnostics& diagnostics, CompilerStage stage, CompilerErrorCode code);

	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage);
	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerErrorCode code);
	[[nodiscard]] const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage, CompilerErrorCode code);
}
