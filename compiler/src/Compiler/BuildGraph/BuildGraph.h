#pragma once

#include "Compiler/Token/Token.h"
#include "Compiler/Module/Module.h"

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
		int m_in_degree = 0;
		bool m_processed = false;
	};

	std::unordered_map<std::string, BuildNode> m_nodes;
	std::unordered_map<std::string, ModuleDeclaration> m_module_declarations;
	std::unordered_map<std::string, std::string> m_module_name_to_file;     // Maps module_name -> file_path (for duplicate detection)
	std::unordered_map<std::string, std::vector<UseImport>> m_use_imports;  // Maps file_name -> use imports

	std::vector<std::string> GetStartingPoints() const;

	void MarkProcessed(const std::string& file_name);

	bool IsComplete() const;

	std::vector<std::vector<std::string>> GetCompilationTiers() const;
};
