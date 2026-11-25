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

struct SymbolDeclaration
{
	SourceSpan m_span;
	std::optional<int> m_documentation_line;
};

struct DefinitionLink
{
	SourceSpan m_reference;
	SymbolDeclaration m_declaration;
};

struct SymbolType
{
	SourceSpan m_span;
	std::string m_type;
	std::optional<int> m_documentation_line;
};

struct EditorSymbols
{
	std::vector<DefinitionLink> m_definitions;
	std::vector<SymbolType> m_types;
};

struct EditorAnalysis
{
	MarmotResult::CompilerReport m_report;
	EditorSymbols m_symbols;
};

using GlobalDeclarations = std::unordered_map<std::string, SymbolDeclaration>;

struct ModuleDefinitions
{
	GlobalDeclarations m_globals;
	std::vector<DefinitionLink> m_local_links;
	std::vector<std::pair<std::string, SourceSpan>> m_global_references;
	std::unordered_map<std::string, std::string> m_reexports;
	std::vector<SymbolType> m_types;

	static ModuleDefinitions Collect(MarmotProgramTree& program, const std::string& module_name, const std::unordered_map<int, std::string>& generic_parameter_names, const std::vector<int>& doc_comment_lines);
};
