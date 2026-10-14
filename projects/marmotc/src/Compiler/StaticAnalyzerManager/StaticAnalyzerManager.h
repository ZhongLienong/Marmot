#pragma once

#include "Compiler/Analysis/WorkerCrossing.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <memory>
#include <vector>

class StaticAnalyzerManager
{
public:
	explicit StaticAnalyzerManager(const WorkerCrossings& crossings);

	std::vector<CompilerWarning> Analyze(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines);

private:
	std::vector<std::unique_ptr<DiagnosticPass>> m_passes;
};
