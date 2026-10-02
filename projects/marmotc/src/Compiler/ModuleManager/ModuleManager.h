#pragma once

#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"
#include "Compiler/Token/Token.h"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

class ModuleSourceLoader;

class ModuleManager
{
private:
	struct DiscoveryState
	{
		std::unordered_set<std::string> m_active_modules;
		std::unordered_set<std::string> m_cyclic_modules;
	};

	enum class StatementType { MODULE, EXPORT, IMPORT, USE };

	struct StatementSpan
	{
		StatementType m_type;
		int m_start;
		int m_end;
		int m_line;
	};

	TokenStream m_main_token_stream;
	std::string m_main_file_name;
	std::vector<std::string> m_main_source_lines;
	CompilationInputs m_inputs;

public:
	ModuleManager(TokenStream&& main_file_tokens, std::string_view main_file_name, std::vector<std::string> main_source_lines = {}, CompilationInputs inputs = {});

	MidoriResult::ModuleManagerResult GenerateBuildGraph();

private:
	MidoriResult::VoidResult GenerateBuildGraphImpl(BuildGraph& build_graph, ModuleSourceLoader& source_loader, DiscoveryState& discovery);

	void CalculateInDegrees(BuildGraph& build_graph);

	std::vector<StatementSpan> ScanModuleStatements(const TokenStream& tokens);

	int ComputeStatementEnd(const TokenStream& tokens, int start, StatementType type);

	MidoriResult::VoidResult ValidateModuleDeclarationPolicy(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const;

	MidoriResult::Result<std::string> ExtractModuleName(const TokenStream& tokens, const StatementSpan& module_span) const;

	MidoriResult::Result<std::tuple<std::string, std::vector<ModuleExport>>> ExtractModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans);

	MidoriResult::Result<std::vector<std::pair<std::string, int>>> ExtractImports(const TokenStream& tokens, const std::vector<StatementSpan>& spans);

	MidoriResult::Result<std::vector<UseImport>> ExtractUseStatements(const TokenStream& tokens, const std::vector<StatementSpan>& spans);

	static void SkipWhiteSpace(const TokenStream& tokens, int& current_index);

	static bool IsKeyword(Token::Name token_name);

};
