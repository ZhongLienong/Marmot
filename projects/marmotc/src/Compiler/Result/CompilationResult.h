#pragma once

#include "Bytecode/Executable/Executable.h"
#include "Compiler/Result/Result.h"

namespace MarmotResult
{
	struct CompiledProgram
	{
		MarmotExecutable m_executable;
		CompilerReport m_report;
		// Every file the program was compiled from, sorted: the entry and every
		// module it reached. What a build depends on.
		std::vector<std::string> m_source_files;
		// Each module's MarmotIR as text, in link order, when --emit-ir asked.
		std::vector<std::string> m_marmot_ir;
		// Each module's checked syntax tree as text, in link order, when --emit-ast asked.
		std::vector<std::string> m_ast;

		CompiledProgram(MarmotExecutable executable, CompilerReport report = {}, std::vector<std::string> source_files = {})
			: m_executable(std::move(executable)),
			m_report(std::move(report)),
			m_source_files(std::move(source_files))
		{
		}

		[[nodiscard]] const CompilerReport& Report() const
		{
			return m_report;
		}

		[[nodiscard]] MarmotExecutable TakeExecutable() &&
		{
			return std::move(m_executable);
		}
	};

	using ExecutableDiagnosticsResult = DiagnosticsResult<MarmotExecutable>;
	using CompiledProgramReportResult = ReportResult<CompiledProgram>;
}
