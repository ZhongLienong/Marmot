#pragma once

#include "Compiler/BuildGraph/BuildGraph.h"
#include "Compiler/CompilationInputs/CompilationInputs.h"
#include "Compiler/Module/Module.h"
#include "Compiler/Result/Result.h"
#include "Compiler/Token/Token.h"
#include "Support/Optional.h"

#include <string>
#include <vector>

namespace MarmotResult
{
	using ModuleManagerResult = Result<BuildGraph>;
}

class ModuleManager
{
public:
	static MarmotResult::ModuleManagerResult GenerateBuildGraph(TokenStream tokens, std::string_view file_name, std::vector<std::string> source_lines, const CompilationInputs& inputs);

private:
	struct DiscoveryState;

	enum class StatementType { MODULE, EXPORT, IMPORT, USE };

	struct StatementSpan
	{
		StatementType m_type;
		int m_start;
		int m_end;
		int m_line;
	};

	struct SourceImport
	{
		std::string m_specifier;
		Token m_token;
		MarmotCompiler::Optional<std::string> m_resolved_path = MarmotCompiler::None;
	};

	TokenStream m_tokens;
	std::string m_file_name;
	std::vector<std::string> m_source_lines;

	ModuleManager(TokenStream tokens, std::string_view file_name, std::vector<std::string> source_lines);

	MarmotResult::VoidResult DiscoverModule(DiscoveryState& discovery);

	std::vector<StatementSpan> ScanModuleStatements(const TokenStream& tokens);

	int ComputeStatementEnd(const TokenStream& tokens, int start, StatementType type);

	MarmotResult::Result<const StatementSpan*> FindModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const;

	MarmotResult::Result<std::string> ExtractModuleName(const TokenStream& tokens, const StatementSpan& module_span) const;

	MarmotResult::Result<ModuleDeclaration> ExtractModuleDeclaration(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const;

	MarmotResult::Result<std::vector<SourceImport>> ExtractImports(const TokenStream& tokens, const std::vector<StatementSpan>& spans) const;

	MarmotResult::Result<std::vector<UseImport>> ExtractUseStatements(const TokenStream& tokens, const std::vector<StatementSpan>& spans);

	static void SkipWhiteSpace(const TokenStream& tokens, int& current_index);

	// Steps past a `{...}` block, nested braces included; false when none starts at `current_index`.
	static bool SkipBracedBlock(const TokenStream& tokens, int& current_index);

};
