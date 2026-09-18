#include "StaticAnalyzerManager.h"

#include "Compiler/StaticAnalyzerManager/Diagnostics/CaptureEscapeDiagnostic/CaptureEscapeDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/CellCrossesWorkerDiagnostic/CellCrossesWorkerDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/IntegerOverflowDiagnostic/IntegerOverflowDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/ShadowingPolicyDiagnostic/ShadowingPolicyDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/UnusedLocalDiagnostic/UnusedLocalDiagnostic.h"

StaticAnalyzerManager::StaticAnalyzerManager()
{
	AddPass(std::make_unique<ShadowingPolicyDiagnostic>());
	AddPass(std::make_unique<UnusedLocalDiagnostic>());
	AddPass(std::make_unique<CaptureEscapeDiagnostic>());
	AddPass(std::make_unique<CellCrossesWorkerDiagnostic>());
	AddPass(std::make_unique<IntegerOverflowDiagnostic>());
}

void StaticAnalyzerManager::AddPass(std::unique_ptr<DiagnosticPass> pass)
{
	m_passes.emplace_back(std::move(pass));
}

StaticAnalysisResult StaticAnalyzerManager::Analyze(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines)
{
	StaticAnalysisContext context
	{
		file_name,
		&source_lines
	};
	DiagnosticSink sink;

	for (const std::unique_ptr<DiagnosticPass>& pass : m_passes)
	{
		pass->Run(program_tree, context, sink);
	}

	return std::move(sink).TakeResult();
}
