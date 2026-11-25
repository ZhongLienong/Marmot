#pragma once

#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"
#include "Compiler/Result/Result.h"

#include <unordered_map>

struct SourceSpan
{
	std::string m_file;
	MarmotSource::Position m_start;
	MarmotSource::Position m_end;
};

struct DefinitionLink
{
	SourceSpan m_reference;
	SourceSpan m_declaration;
};

struct EditorAnalysis
{
	MarmotResult::CompilerReport m_report;
	std::vector<DefinitionLink> m_definitions;
};

using GlobalDeclarations = std::unordered_map<std::string, SourceSpan>;

struct ModuleDefinitions
{
	GlobalDeclarations m_globals;
	std::vector<DefinitionLink> m_local_links;
	std::vector<std::pair<std::string, SourceSpan>> m_global_references;
	std::unordered_map<std::string, std::string> m_reexports;

	static ModuleDefinitions Collect(MarmotProgramTree& program, const std::string& module_name);
};
