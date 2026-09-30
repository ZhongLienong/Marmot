#include "support/DiagnosticMatchers.h"

#include <format>
#include <ranges>

namespace
{
	template<typename Diagnostic, typename StageType, typename CodeType>
	bool MatchesDiagnostic(
		const Diagnostic& diagnostic,
		const std::optional<StageType>& expected_stage,
		const std::optional<CodeType>& expected_code,
		const std::optional<int>& expected_line,
		const std::vector<std::string>& message_substrings,
		const std::vector<std::string>& rendered_substrings,
		std::string* mismatch)
	{
		auto set_mismatch = [mismatch](std::string message)
		{
			if (mismatch != nullptr)
			{
				*mismatch = std::move(message);
			}
		};

		if (expected_stage.has_value() && diagnostic.m_stage != expected_stage.value())
		{
			set_mismatch(std::format("Unexpected stage: expected {}, got {}", static_cast<int>(expected_stage.value()), static_cast<int>(diagnostic.m_stage)));
			return false;
		}

		if (expected_code.has_value() && diagnostic.m_code != expected_code.value())
		{
			set_mismatch(std::format("Unexpected diagnostic code: expected {}, got {}", static_cast<int>(expected_code.value()), static_cast<int>(diagnostic.m_code)));
			return false;
		}

		if (expected_line.has_value())
		{
			if (!diagnostic.m_location.has_value())
			{
				set_mismatch("Diagnostic did not include a source location.");
				return false;
			}

			if (diagnostic.m_location->m_line != expected_line.value())
			{
				set_mismatch(std::format("Unexpected line: expected {}, got {}", expected_line.value(), diagnostic.m_location->m_line));
				return false;
			}
		}

		for (const std::string& substring : message_substrings)
		{
			if (diagnostic.m_message.find(substring) == std::string::npos)
			{
				set_mismatch(std::format("Diagnostic message did not contain '{}'. Actual message: {}", substring, diagnostic.m_message));
				return false;
			}
		}

		const std::string stripped_rendered = MidoriTest::StripAnsiCodes(diagnostic.Rendered());
		for (const std::string& substring : rendered_substrings)
		{
			if (stripped_rendered.find(substring) == std::string::npos)
			{
				set_mismatch(std::format("Rendered diagnostic did not contain '{}'. Actual render: {}", substring, stripped_rendered));
				return false;
			}
		}

		return true;
	}
}

namespace MidoriTest
{
	std::string StripAnsiCodes(std::string_view text)
	{
		std::string stripped;
		stripped.reserve(text.size());

		bool in_escape = false;
		for (const char ch : text)
		{
			if (ch == '\033')
			{
				in_escape = true;
				continue;
			}

			if (in_escape)
			{
				if (ch == 'm')
				{
					in_escape = false;
				}
				continue;
			}

			stripped.push_back(ch);
		}

		return stripped;
	}

	bool Matches(const CompilerWarning& warning, const WarningExpectation& expectation, std::string* mismatch)
	{
		return MatchesDiagnostic(
			warning,
			expectation.m_stage,
			expectation.m_code,
			expectation.m_line,
			expectation.m_message_substrings,
			expectation.m_rendered_substrings,
			mismatch);
	}

	bool Matches(const CompilerError& error, const ErrorExpectation& expectation, std::string* mismatch)
	{
		return MatchesDiagnostic(
			error,
			expectation.m_stage,
			expectation.m_code,
			expectation.m_line,
			expectation.m_message_substrings,
			expectation.m_rendered_substrings,
			mismatch);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerWarningCode code)
	{
		return FindWarning(warnings.Warnings(), code);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerStage stage)
	{
		return FindWarning(warnings.Warnings(), stage);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerWarnings& warnings, CompilerStage stage, CompilerWarningCode code)
	{
		return FindWarning(warnings.Warnings(), stage, code);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerWarningCode code)
	{
		return FindWarning(report.Warnings(), code);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerStage stage)
	{
		return FindWarning(report.Warnings(), stage);
	}

	const CompilerWarning* FindWarning(const MidoriResult::CompilerReport& report, CompilerStage stage, CompilerWarningCode code)
	{
		return FindWarning(report.Warnings(), stage, code);
	}

	const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerWarningCode code)
	{
		const std::vector<CompilerWarning>::const_iterator warning_it = std::ranges::find_if
		(
			warnings,
			[code](const CompilerWarning& warning)
			{
				return warning.m_code == code;
			}
		);

		if (warning_it == warnings.cend())
		{
			return nullptr;
		}

		return std::addressof(*warning_it);
	}

	const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage)
	{
		const std::vector<CompilerWarning>::const_iterator warning_it = std::ranges::find_if
		(
			warnings,
			[stage](const CompilerWarning& warning)
			{
				return warning.m_stage == stage;
			}
		);

		if (warning_it == warnings.cend())
		{
			return nullptr;
		}

		return std::addressof(*warning_it);
	}

	const CompilerWarning* FindWarning(const std::vector<CompilerWarning>& warnings, CompilerStage stage, CompilerWarningCode code)
	{
		const std::vector<CompilerWarning>::const_iterator warning_it = std::ranges::find_if
		(
			warnings,
			[stage, code](const CompilerWarning& warning)
			{
				return warning.m_stage == stage && warning.m_code == code;
			}
		);

		if (warning_it == warnings.cend())
		{
			return nullptr;
		}

		return std::addressof(*warning_it);
	}

	const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerStage stage)
	{
		return FindError(diagnostics.m_errors, stage);
	}

	const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerErrorCode code)
	{
		return FindError(diagnostics.m_errors, code);
	}

	const CompilerError* FindError(const MidoriResult::CompilerDiagnostics& diagnostics, CompilerStage stage, CompilerErrorCode code)
	{
		return FindError(diagnostics.m_errors, stage, code);
	}

	const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage)
	{
		const std::vector<CompilerError>::const_iterator error_it = std::ranges::find_if
		(
			errors,
			[stage](const CompilerError& error)
			{
				return error.m_stage == stage;
			}
		);

		if (error_it == errors.cend())
		{
			return nullptr;
		}

		return std::addressof(*error_it);
	}

	const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerErrorCode code)
	{
		const std::vector<CompilerError>::const_iterator error_it = std::ranges::find_if
		(
			errors,
			[code](const CompilerError& error)
			{
				return error.m_code == code;
			}
		);

		if (error_it == errors.cend())
		{
			return nullptr;
		}

		return std::addressof(*error_it);
	}

	const CompilerError* FindError(const std::vector<CompilerError>& errors, CompilerStage stage, CompilerErrorCode code)
	{
		const std::vector<CompilerError>::const_iterator error_it = std::ranges::find_if
		(
			errors,
			[stage, code](const CompilerError& error)
			{
				return error.m_stage == stage && error.m_code == code;
			}
		);

		if (error_it == errors.cend())
		{
			return nullptr;
		}

		return std::addressof(*error_it);
	}
}
