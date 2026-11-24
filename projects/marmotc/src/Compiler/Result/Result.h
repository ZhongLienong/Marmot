#pragma once

#include <algorithm>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Compiler/Error/CompilerError.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

namespace MarmotResult
{
	// Diagnostics of one kind, in the order they were reported.
	template<typename Diagnostic>
	struct DiagnosticList
	{
		std::vector<Diagnostic> m_items;

		DiagnosticList() = default;

		explicit DiagnosticList(Diagnostic diagnostic)
			: m_items{ std::move(diagnostic) }
		{
		}

		explicit DiagnosticList(std::vector<Diagnostic>&& diagnostics)
			: m_items(std::move(diagnostics))
		{
		}

		[[nodiscard]] bool Empty() const
		{
			return m_items.empty();
		}

		[[nodiscard]] size_t Size() const
		{
			return m_items.size();
		}

		[[nodiscard]] const Diagnostic& First() const
		{
			return m_items.front();
		}

		[[nodiscard]] std::vector<Diagnostic> TakeAll() &&
		{
			return std::move(m_items);
		}

		[[nodiscard]] Diagnostic TakeFirst() &&
		{
			return std::move(m_items.front());
		}

		void Append(std::vector<Diagnostic> diagnostics)
		{
			std::ranges::move(diagnostics, std::back_inserter(m_items));
		}

		void Append(DiagnosticList diagnostics)
		{
			Append(std::move(diagnostics.m_items));
		}

		[[nodiscard]] std::string Rendered() const
		{
			std::string rendered;
			for (const Diagnostic& diagnostic : m_items)
			{
				rendered.append(diagnostic.Rendered());
				if (!rendered.empty() && rendered.back() != '\n')
				{
					rendered.push_back('\n');
				}
			}

			return rendered;
		}

	};

	using CompilerWarnings = DiagnosticList<CompilerWarning>;
	using CompilerDiagnostics = DiagnosticList<CompilerError>;

	struct CompilerReport
	{
		CompilerWarnings m_warnings;
		CompilerDiagnostics m_errors;

		CompilerReport() = default;

		explicit CompilerReport(CompilerWarnings warnings)
			: m_warnings(std::move(warnings))
		{
		}

		explicit CompilerReport(CompilerError error)
			: m_errors(std::move(error))
		{
		}

		explicit CompilerReport(CompilerDiagnostics errors)
			: m_errors(std::move(errors))
		{
		}

		CompilerReport(CompilerWarnings warnings, CompilerDiagnostics errors)
			: m_warnings(std::move(warnings)),
			m_errors(std::move(errors))
		{
		}

		void Append(CompilerReport report)
		{
			m_warnings.Append(std::move(report.m_warnings));
			m_errors.Append(std::move(report.m_errors));
		}

		// The warnings under one header for each file they are in.
		[[nodiscard]] std::string RenderedWarnings() const
		{
			const auto warning_file_path = [](const CompilerWarning& warning) -> std::string_view
			{
				if (!warning.m_location.has_value() || warning.m_location->m_file_name.empty())
				{
					return {};
				}

				return warning.m_location->m_file_name;
			};

			const std::vector<CompilerWarning>& warnings = m_warnings.m_items;
			std::string rendered;
			size_t group_begin = 0u;
			while (group_begin < warnings.size())
			{
				const std::string_view file_path = warning_file_path(warnings[group_begin]);
				size_t group_end = group_begin + 1u;
				while (group_end < warnings.size() && warning_file_path(warnings[group_end]) == file_path)
				{
					group_end += 1u;
				}

				if (!file_path.empty())
				{
					rendered += RenderWarningGroupHeader(group_end - group_begin, file_path);
				}

				for (size_t index = group_begin; index < group_end; index += 1u)
				{
					rendered.append(warnings[index].Rendered());
					if (!rendered.empty() && rendered.back() != '\n')
					{
						rendered.push_back('\n');
					}
				}

				group_begin = group_end;
			}

			return rendered;
		}

		[[nodiscard]] std::string Rendered() const
		{
			return RenderedWarnings() + m_errors.Rendered();
		}

		[[nodiscard]] std::string MachineReadableWarnings() const
		{
			std::string serialized;
			for (const CompilerWarning& warning : m_warnings.m_items)
			{
				serialized += SerializeMachineReadableWarning(warning);
				serialized.push_back('\n');
			}

			return serialized;
		}

		[[nodiscard]] std::string MachineReadableJson() const;
	};

	template<typename ValueType>
	using ReportResult = std::expected<ValueType, CompilerReport>;

	template<typename ValueType>
	using Result = std::expected<ValueType, CompilerError>;

	template<typename ValueType>
	using DiagnosticsResult = std::expected<ValueType, CompilerDiagnostics>;

	using TokenResult = Result<Token>;
	using TokenListResult = Result<std::vector<Token>>;
	using FunctionParamResult = Result<std::pair<Token, std::shared_ptr<MarmotType>>>;
	using FunctionParamsResult = Result<std::vector<std::pair<Token, std::shared_ptr<MarmotType>>>>;
	using LexerResult = Result<TokenStream>;
	using ExpressionResult = Result<std::unique_ptr<MarmotExpression>>;
	using PatternResult = Result<std::unique_ptr<MarmotPattern>>;
	using StatementResult = Result<std::unique_ptr<MarmotStatement>>;
	using ParserResult = DiagnosticsResult<MarmotProgramTree>;
	using TypeResult = Result<std::shared_ptr<MarmotType>>;
	using TypeListResult = Result<std::vector<std::shared_ptr<MarmotType>>>;
	using TypeCheckerResult = DiagnosticsResult<MarmotProgramTree>;

	using VoidResult = Result<void>;
}
