#include "BaseDiagnosticPass.h"

#include <utility>

std::vector<CompilerWarning> DiagnosticPass::Run(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines)
{
	m_file_name = file_name;
	m_source_lines = &source_lines;
	Reset();
	VisitProgram(program_tree);
	Finish();
	m_source_lines = nullptr;
	return std::exchange(m_warnings, {});
}

void DiagnosticPass::Reset()
{
}

void DiagnosticPass::Finish()
{
}

void DiagnosticPass::EmitWarning(CompilerWarningCode code, const Token& token, std::string_view message, std::optional<std::string_view> suggestion)
{
	m_warnings.emplace_back(CompilerWarning::WithToken(CompilerStage::StaticAnalyzer, message, token, m_file_name, *m_source_lines, suggestion, code));
}
