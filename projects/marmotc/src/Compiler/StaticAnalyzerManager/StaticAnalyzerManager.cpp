#include "StaticAnalyzerManager.h"

#include "Compiler/StaticAnalyzerManager/Diagnostics/CellCrossesWorkerDiagnostic/CellCrossesWorkerDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/IntegerOverflowDiagnostic/IntegerOverflowDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/ShadowingPolicyDiagnostic/ShadowingPolicyDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/UnusedLocalDiagnostic/UnusedLocalDiagnostic.h"

#include <algorithm>
#include <iterator>

StaticAnalyzerManager::StaticAnalyzerManager()
{
	m_passes.emplace_back(std::make_unique<ShadowingPolicyDiagnostic>());
	m_passes.emplace_back(std::make_unique<UnusedLocalDiagnostic>());
	m_passes.emplace_back(std::make_unique<CellCrossesWorkerDiagnostic>());
	m_passes.emplace_back(std::make_unique<IntegerOverflowDiagnostic>());
}

std::vector<CompilerWarning> StaticAnalyzerManager::Analyze(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines)
{
	std::vector<CompilerWarning> warnings;
	for (const std::unique_ptr<DiagnosticPass>& pass : m_passes)
	{
		std::ranges::move(pass->Run(program_tree, file_name, source_lines), std::back_inserter(warnings));
	}
	return warnings;
}
