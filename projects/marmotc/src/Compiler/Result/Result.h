#pragma once

#include <algorithm>
#include <concepts>
#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Compiler/Error/CompilerError.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/BytecodeModule/BytecodeModule.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/BuildGraph/BuildGraph.h"

namespace MidoriResult
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

		[[nodiscard]] std::string MachineReadableJson() const
		{
			std::string serialized = "[";
			for (size_t index = 0u; index < m_items.size(); index += 1u)
			{
				if (index > 0u)
				{
					serialized.push_back(',');
				}

				if constexpr (std::same_as<Diagnostic, CompilerWarning>)
				{
					serialized += SerializeMachineReadableWarningPayload(m_items[index]);
				}
				else
				{
					serialized += SerializeMachineReadableError(m_items[index]);
				}
			}

			serialized.push_back(']');
			return serialized;
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

		[[nodiscard]] std::string MachineReadableJson() const
		{
			std::string diagnostics = "[";
			for (size_t index = 0u; index < m_warnings.m_items.size(); index += 1u)
			{
				if (index > 0u)
				{
					diagnostics.push_back(',');
				}

				diagnostics += SerializeMachineReadableWarningPayload(m_warnings.m_items[index]);
			}

			for (size_t index = 0u; index < m_errors.m_items.size(); index += 1u)
			{
				if (!m_warnings.Empty() || index > 0u)
				{
					diagnostics.push_back(',');
				}

				diagnostics += SerializeMachineReadableError(m_errors.m_items[index]);
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
		// Every file the program was compiled from, sorted: the entry and every
		// module it reached. What a build depends on.
		std::vector<std::string> m_source_files;
		// Each module's MidoriIR as text, in link order, when --emit-ir asked.
		std::vector<std::string> m_midori_ir;
		// Each module's checked syntax tree as text, in link order, when --emit-ast asked.
		std::vector<std::string> m_ast;

		CompiledProgram(MidoriExecutable executable, CompilerReport report = {}, std::vector<std::string> source_files = {})
			: m_executable(std::move(executable)),
			m_report(std::move(report)),
			m_source_files(std::move(source_files))
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
	using BytecodeBackendResult = DiagnosticsResult<BytecodeModule>;
	using CompiledModuleReportResult = ReportResult<CompiledModule>;
	using CompilerResult = DiagnosticsResult<MidoriExecutable>;
	using CompilationResult = ReportResult<CompiledProgram>;

	using VoidResult = Result<void>;
}
