#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/Error/CompilerError.h"

#include <string>
#include <string_view>
#include <vector>

class StaticAnalyzerManager
{
public:
	std::vector<CompilerWarning> Analyze(MarmotProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines);
};
