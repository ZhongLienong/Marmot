#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Token/Token.h"
#include "Compiler/Module/Module.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct BuildGraph
{
	struct BuildNode
	{
		TokenStream m_tokens;
		std::string m_file_name;
		std::vector<std::string> m_source_lines;
		std::vector<std::string> m_dependencies;
		std::vector<UseImport> m_use_imports;  // Symbols brought into scope via 'use' statements
		// Each dependency's first import, where a diagnostic about it points.
		std::unordered_map<std::string, Token> m_import_tokens;
	};

	std::unordered_map<std::string, BuildNode> m_nodes;
	std::unordered_map<std::string, ModuleDeclaration> m_module_declarations;
	std::unordered_map<std::string, std::string> m_module_name_to_file;     // Maps module_name -> file_path (for duplicate detection)

	std::vector<std::vector<std::string>> GetCompilationTiers() const;
};
