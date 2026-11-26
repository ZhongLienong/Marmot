#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Support/Optional.h"

#include <string>
#include <string_view>
#include <vector>

// One walk over a checked module that reports warnings.
class DiagnosticPass : protected MarmotAbstractSyntaxTreeWalker
{
public:
	std::vector<CompilerWarning> Run(MarmotProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines);

protected:
	virtual void Finish();

	void EmitWarning(CompilerWarningCode code, const Token& token, std::string_view message, MarmotCompiler::Optional<std::string_view> suggestion = MarmotCompiler::None);

private:
	std::string_view m_file_name;
	const std::vector<std::string>* m_source_lines = nullptr;
	std::vector<CompilerWarning> m_warnings;
};
