#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

#include <string>
#include <string_view>

// The checked tree of one module as indented text, which --emit-ast prints.
class AbstractSyntaxTreePrinter
{
private:
	std::string_view m_module_name;
	const MidoriProgramTree& m_program;

public:
	AbstractSyntaxTreePrinter(std::string_view module_name, const MidoriProgramTree& program);

	[[nodiscard]] std::string Print() const;
};
