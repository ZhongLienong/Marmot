#include "StaticAnalyzerManager.h"

#include "Compiler/StaticAnalyzerManager/Diagnostics/IntegerOverflowDiagnostic/IntegerOverflowDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/ShadowingPolicyDiagnostic/ShadowingPolicyDiagnostic.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/UnusedLocalDiagnostic/UnusedLocalDiagnostic.h"

#include <algorithm>
#include <iterator>

std::vector<CompilerWarning> StaticAnalyzerManager::Analyze(MarmotProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines)
{
	ShadowingPolicyDiagnostic shadowing;
	UnusedLocalDiagnostic unused_locals;
	IntegerOverflowDiagnostic integer_overflow;
	DiagnosticPass* const passes[] = { &shadowing, &unused_locals, &integer_overflow };
	std::vector<CompilerWarning> warnings;
	for (DiagnosticPass* pass : passes)
	{
		std::ranges::move(pass->Run(program_tree, file_name, source_lines), std::back_inserter(warnings));
	}
	return warnings;
}
