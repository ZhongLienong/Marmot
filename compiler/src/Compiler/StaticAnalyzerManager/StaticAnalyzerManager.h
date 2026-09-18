#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <memory>
#include <vector>

class StaticAnalyzerManager
{
public:
	StaticAnalyzerManager();

	void AddPass(std::unique_ptr<DiagnosticPass> pass);

	StaticAnalysisResult Analyze(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines);

private:
	std::vector<std::unique_ptr<DiagnosticPass>> m_passes;
};
