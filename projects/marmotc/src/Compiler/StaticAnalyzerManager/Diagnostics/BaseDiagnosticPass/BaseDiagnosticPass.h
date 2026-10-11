#pragma once

#include "Compiler/Error/CompilerError.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// One walk over a checked module that reports warnings.
class DiagnosticPass : protected MidoriAbstractSyntaxTreeWalker
{
public:
	std::vector<CompilerWarning> Run(MidoriProgramTree& program_tree, std::string_view file_name, const std::vector<std::string>& source_lines);

protected:
	virtual void Reset(MidoriProgramTree& program_tree);

	virtual void Finish();

	void EmitWarning(CompilerWarningCode code, const Token& token, std::string_view message, std::optional<std::string_view> suggestion = std::nullopt);

private:
	std::string_view m_file_name;
	const std::vector<std::string>* m_source_lines = nullptr;
	std::vector<CompilerWarning> m_warnings;
};
