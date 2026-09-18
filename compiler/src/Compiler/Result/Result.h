#pragma once

#include <expected>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/Error/Error.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/BuildGraph/BuildGraph.h"

namespace MidoriResult
{
	struct CompilerWarnings
	{
		std::vector<CompilerWarning> m_warnings;

		CompilerWarnings() = default;

		explicit CompilerWarnings(CompilerWarning warning)
			: m_warnings{ std::move(warning) }
		{
		}

		explicit CompilerWarnings(std::vector<CompilerWarning>&& warnings)
			: m_warnings(std::move(warnings))
		{
		}

		[[nodiscard]] bool Empty() const
		{
			return m_warnings.empty();
		}

		[[nodiscard]] size_t Size() const
		{
			return m_warnings.size();
		}

		[[nodiscard]] const CompilerWarning& First() const
		{
			return m_warnings.front();
		}

		[[nodiscard]] const std::vector<CompilerWarning>& Warnings() const
		{
			return m_warnings;
		}

		[[nodiscard]] std::vector<CompilerWarning> TakeAll() &&
		{
			return std::move(m_warnings);
		}

		[[nodiscard]] CompilerWarning TakeFirst() &&
		{
			return std::move(m_warnings.front());
		}

		void Append(CompilerWarning warning)
		{
			m_warnings.emplace_back(std::move(warning));
		}

		void Append(const std::vector<CompilerWarning>& warnings)
		{
			m_warnings.insert(m_warnings.end(), warnings.begin(), warnings.end());
		}

		void Append(std::vector<CompilerWarning>&& warnings)
		{
			for (CompilerWarning& warning : warnings)
			{
				m_warnings.emplace_back(std::move(warning));
			}
		}

		void Append(const CompilerWarnings& warnings)
		{
			Append(warnings.m_warnings);
		}

		void Append(CompilerWarnings&& warnings)
		{
			Append(std::move(warnings.m_warnings));
		}

		[[nodiscard]] std::string Rendered() const
		{
			std::string rendered;
			for (const CompilerWarning& warning : m_warnings)
			{
				const std::string_view warning_rendered = warning.Rendered();
				rendered.append(warning_rendered);
				if (!rendered.empty() && rendered.back() != '\n')
				{
					rendered.push_back('\n');
				}
			}

			return rendered;
		}

		[[nodiscard]] std::string RenderedReport() const
		{
			if (m_warnings.empty())
			{
				return {};
			}

			const auto warning_file_path = [](const CompilerWarning& warning) -> std::string_view
			{
				if (!warning.m_location.has_value() || warning.m_location->m_file_name.empty())
				{
					return {};
				}

				return warning.m_location->m_file_name;
			};

			std::string rendered;
			size_t group_begin = 0u;
			while (group_begin < m_warnings.size())
			{
				const std::string_view file_path = warning_file_path(m_warnings[group_begin]);
				size_t group_end = group_begin + 1u;
				while (group_end < m_warnings.size() && warning_file_path(m_warnings[group_end]) == file_path)
				{
					group_end += 1u;
				}

				if (!file_path.empty())
				{
					rendered += RenderWarningGroupHeader(group_end - group_begin, file_path);
				}

				for (size_t index = group_begin; index < group_end; index += 1u)
				{
					const std::string_view warning_rendered = m_warnings[index].Rendered();
					rendered.append(warning_rendered);
					if (!rendered.empty() && rendered.back() != '\n')
					{
						rendered.push_back('\n');
					}
				}

				group_begin = group_end;
			}

			return rendered;
		}

		[[nodiscard]] std::string MachineReadable() const
		{
			std::string serialized;
			for (const CompilerWarning& warning : m_warnings)
			{
				serialized += SerializeMachineReadableWarning(warning);
				serialized.push_back('\n');
			}

			return serialized;
		}

		[[nodiscard]] std::string MachineReadableJson() const
		{
			std::string serialized = "[";
			for (size_t index = 0u; index < m_warnings.size(); index += 1u)
			{
				if (index > 0u)
				{
					serialized.push_back(',');
				}

				serialized += SerializeMachineReadableWarningPayload(m_warnings[index]);
			}

			serialized.push_back(']');
			return serialized;
		}
	};

	struct CompilerDiagnostics
	{
		std::vector<CompilerError> m_errors;

		CompilerDiagnostics() = default;

		explicit CompilerDiagnostics(CompilerError error)
			: m_errors{ std::move(error) }
		{
		}

		explicit CompilerDiagnostics(std::vector<CompilerError>&& errors)
			: m_errors(std::move(errors))
		{
		}

		[[nodiscard]] bool Empty() const
		{
			return m_errors.empty();
		}

		[[nodiscard]] size_t Size() const
		{
			return m_errors.size();
		}

		[[nodiscard]] const CompilerError& First() const
		{
			return m_errors.front();
		}

		[[nodiscard]] const std::vector<CompilerError>& Errors() const
		{
			return m_errors;
		}

		[[nodiscard]] std::vector<CompilerError> TakeAll() &&
		{
			return std::move(m_errors);
		}

		[[nodiscard]] CompilerError TakeFirst() &&
		{
			return std::move(m_errors.front());
		}

		void Append(CompilerError error)
		{
			m_errors.emplace_back(std::move(error));
		}

		void Append(const std::vector<CompilerError>& errors)
		{
			m_errors.insert(m_errors.end(), errors.begin(), errors.end());
		}

		void Append(std::vector<CompilerError>&& errors)
		{
			for (CompilerError& error : errors)
			{
				m_errors.emplace_back(std::move(error));
			}
		}

		void Append(const CompilerDiagnostics& diagnostics)
		{
			Append(diagnostics.m_errors);
		}

		void Append(CompilerDiagnostics&& diagnostics)
		{
			Append(std::move(diagnostics.m_errors));
		}

		[[nodiscard]] std::string Rendered() const
		{
			std::string rendered;
			for (const CompilerError& error : m_errors)
			{
				const std::string_view error_rendered = error.Rendered();
				rendered.append(error_rendered);
				if (!rendered.empty() && rendered.back() != '\n')
				{
					rendered.push_back('\n');
				}
			}

			return rendered;
		}

		[[nodiscard]] std::string MachineReadableJson() const
		{
			std::string serialized = "[";
			for (size_t index = 0u; index < m_errors.size(); index += 1u)
			{
				if (index > 0u)
				{
					serialized.push_back(',');
				}

				serialized += SerializeMachineReadableError(m_errors[index]);
			}

			serialized.push_back(']');
			return serialized;
		}
	};

	struct CompilerReport
	{
		CompilerWarnings m_warnings;
		CompilerDiagnostics m_errors;

		CompilerReport() = default;

		explicit CompilerReport(CompilerWarning warning)
			: m_warnings(std::move(warning))
		{
		}

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

		[[nodiscard]] bool Empty() const
		{
			return m_warnings.Empty() && m_errors.Empty();
		}

		[[nodiscard]] bool HasWarnings() const
		{
			return !m_warnings.Empty();
		}

		[[nodiscard]] bool HasErrors() const
		{
			return !m_errors.Empty();
		}

		[[nodiscard]] size_t WarningCount() const
		{
			return m_warnings.Size();
		}

		[[nodiscard]] size_t ErrorCount() const
		{
			return m_errors.Size();
		}

		[[nodiscard]] const CompilerWarnings& Warnings() const
		{
			return m_warnings;
		}

		[[nodiscard]] const CompilerDiagnostics& Errors() const
		{
			return m_errors;
		}

		[[nodiscard]] CompilerWarnings TakeWarnings() &&
		{
			return std::move(m_warnings);
		}

		[[nodiscard]] CompilerDiagnostics TakeErrors() &&
		{
			return std::move(m_errors);
		}

		CompilerReport& AppendWarnings(const CompilerWarnings& warnings)
		{
			m_warnings.Append(warnings);
			return *this;
		}

		CompilerReport& AppendWarnings(CompilerWarnings&& warnings)
		{
			m_warnings.Append(std::move(warnings));
			return *this;
		}

		CompilerReport& AppendWarnings(const std::vector<CompilerWarning>& warnings)
		{
			m_warnings.Append(warnings);
			return *this;
		}

		CompilerReport& AppendWarnings(std::vector<CompilerWarning>&& warnings)
		{
			m_warnings.Append(std::move(warnings));
			return *this;
		}

		CompilerReport& AppendErrors(const CompilerDiagnostics& errors)
		{
			m_errors.Append(errors);
			return *this;
		}

		CompilerReport& AppendErrors(CompilerDiagnostics&& errors)
		{
			m_errors.Append(std::move(errors));
			return *this;
		}

		CompilerReport& Append(CompilerReport&& report)
		{
			AppendWarnings(std::move(report.m_warnings));
			AppendErrors(std::move(report.m_errors));
			return *this;
		}

		[[nodiscard]] std::string RenderedWarnings() const
		{
			return m_warnings.RenderedReport();
		}

		[[nodiscard]] std::string RenderedErrors() const
		{
			return m_errors.Rendered();
		}

		[[nodiscard]] std::string Rendered() const
		{
			std::string rendered = RenderedWarnings();
			rendered += RenderedErrors();
			return rendered;
		}

		[[nodiscard]] std::string MachineReadableWarnings() const
		{
			return m_warnings.MachineReadable();
		}

		[[nodiscard]] std::string MachineReadableJson() const
		{
			std::string diagnostics = "[";
			const std::vector<CompilerWarning>& warnings = m_warnings.Warnings();
			for (size_t index = 0u; index < warnings.size(); index += 1u)
			{
				if (index > 0u)
				{
					diagnostics.push_back(',');
				}

				diagnostics += SerializeMachineReadableWarningPayload(warnings[index]);
			}

			const std::vector<CompilerError>& errors = m_errors.Errors();
			for (size_t index = 0u; index < errors.size(); index += 1u)
			{
				if (!warnings.empty() || index > 0u)
				{
					diagnostics.push_back(',');
				}

				diagnostics += SerializeMachineReadableError(errors[index]);
			}

			diagnostics.push_back(']');

			return std::string("{\"version\":1,\"source\":\"marmot\",\"diagnostics\":") + diagnostics +
				",\"warnings\":" + m_warnings.MachineReadableJson() +
				",\"errors\":" + m_errors.MachineReadableJson() + "}";
		}
	};

	template<typename ValueType>
	using ReportResult = std::expected<ValueType, CompilerReport>;

	struct CompiledProgram
	{
		MidoriExecutable m_executable;
		CompilerReport m_report;

		CompiledProgram(MidoriExecutable executable, CompilerReport report = {})
			: m_executable(std::move(executable)),
			m_report(std::move(report))
		{
		}

		[[nodiscard]] const CompilerReport& Report() const
		{
			return m_report;
		}

		[[nodiscard]] MidoriExecutable TakeExecutable() &&
		{
			return std::move(m_executable);
		}
	};

	template<typename ValueType>
	using Result = std::expected<ValueType, CompilerError>;

	template<typename ValueType>
	using DiagnosticsResult = std::expected<ValueType, CompilerDiagnostics>;

	using Error = CompilerError;
	using TokenResult = Result<Token>;
	using TokenListResult = Result<std::vector<Token>>;
	using FunctionParamResult = Result<std::pair<Token, std::shared_ptr<MidoriType>>>;
	using FunctionParamsResult = Result<std::vector<std::pair<Token, std::shared_ptr<MidoriType>>>>;
	using LexerResult = Result<TokenStream>;
	using ModuleManagerResult = Result<BuildGraph>;
	using BytecodeLinkerResult = Result<MidoriExecutable>;
	using ExpressionResult = Result<std::unique_ptr<MidoriExpression>>;
	using PatternResult = Result<std::unique_ptr<MidoriPattern>>;
	using StatementResult = Result<std::unique_ptr<MidoriStatement>>;
	using ParserResult = DiagnosticsResult<MidoriProgramTree>;
	using TypeResult = Result<std::shared_ptr<MidoriType>>;
	using TypeListResult = Result<std::vector<std::shared_ptr<MidoriType>>>;
	using TypeCheckerResult = DiagnosticsResult<MidoriProgramTree>;
	using OptimizerResult = Result<MidoriProgramTree>;
	using CodeGeneratorResult = DiagnosticsResult<BytecodeModule>;
	using CompiledModuleResult = DiagnosticsResult<CompiledModule>;
	using CompiledModuleReportResult = ReportResult<CompiledModule>;
	using CompilerResult = DiagnosticsResult<MidoriExecutable>;
	using CompilationResult = ReportResult<CompiledProgram>;

	// Generic result types
	using VoidResult = Result<void>;
	using VoidResultList = std::vector<VoidResult>;

	// Async compilation result types
	using FutureModuleResult = std::future<CompiledModuleResult>;
}
