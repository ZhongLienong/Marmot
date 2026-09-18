#include "BaseDiagnosticPass.h"

const std::vector<std::string>& StaticAnalysisContext::SourceLines() const
{
	return *m_source_lines;
}

void DiagnosticSink::AddWarning(CompilerWarning warning)
{
	m_warnings.emplace_back(std::move(warning));
}

void DiagnosticSink::AddError(CompilerError error)
{
	m_errors.emplace_back(std::move(error));
}

StaticAnalysisResult DiagnosticSink::TakeResult() &&
{
	return StaticAnalysisResult
	{
		std::move(m_warnings),
		std::move(m_errors)
	};
}

void AstDiagnosticPass::Run(MidoriProgramTree& program_tree, const StaticAnalysisContext& context, DiagnosticSink& sink)
{
	m_context = &context;
	m_sink = &sink;
	Reset();
	VisitProgram(program_tree);
	Finish();
	m_sink = nullptr;
	m_context = nullptr;
}

void AstDiagnosticPass::Reset()
{
}

void AstDiagnosticPass::Finish()
{
}

const StaticAnalysisContext& AstDiagnosticPass::Context() const
{
	return *m_context;
}

DiagnosticSink& AstDiagnosticPass::Sink() const
{
	return *m_sink;
}

void AstDiagnosticPass::EmitWarning(CompilerWarningCode code, const Token& token, std::string_view message, std::optional<std::string_view> suggestion)
{
	Sink().AddWarning(CompilerWarning::WithToken(CompilerStage::StaticAnalyzer, message, token, Context().m_file_name, Context().SourceLines(), suggestion, code));
}
