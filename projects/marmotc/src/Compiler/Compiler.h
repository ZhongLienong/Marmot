#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Result/Result.h"
#include <string>
#include <vector>

class CompilationTimings;

class Compiler
{
private:
	std::string m_source_code;
	std::string m_file_name;
	std::vector<std::string> m_source_lines;
	CompilationInputs m_inputs;

	MarmotResult::CompilationResult CompileWithTimings(CompilationTimings& timings);

public:
	Compiler(std::string&& source_code, std::string&& file_name, CompilationInputs inputs = {});

	MarmotResult::CompilationResult CompileWithReport();
	MarmotResult::CompilerResult Compile();
};
