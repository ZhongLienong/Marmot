#pragma once

#include "Common/Error/Error.h"
#include "Compiler/Analysis/AbstractSyntaxTreeWalker.h"
#include "Compiler/AbstractSyntaxTree/AbstractSyntaxTree.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct StaticAnalysisContext
{
	std::string_view m_file_name;
	const std::vector<std::string>* m_source_lines = nullptr;

	const std::vector<std::string>& SourceLines() const;
};

struct StaticAnalysisResult
{
	std::vector<CompilerWarning> m_warnings;
	std::vector<CompilerError> m_errors;
};

class DiagnosticSink
{
public:
	void AddWarning(CompilerWarning warning);

	void AddError(CompilerError error);

	StaticAnalysisResult TakeResult() &&;

private:
	std::vector<CompilerWarning> m_warnings;
	std::vector<CompilerError> m_errors;
};

class DiagnosticPass
{
public:
	virtual ~DiagnosticPass() = default;

	virtual std::string_view GetName() const = 0;

	virtual void Run(MidoriProgramTree& program_tree, const StaticAnalysisContext& context, DiagnosticSink& sink) = 0;
};

class AstDiagnosticPass : public DiagnosticPass, protected MidoriAbstractSyntaxTreeWalker
{
public:
	void Run(MidoriProgramTree& program_tree, const StaticAnalysisContext& context, DiagnosticSink& sink) override;

protected:
	virtual void Reset();

	virtual void Finish();

	const StaticAnalysisContext& Context() const;

	DiagnosticSink& Sink() const;

	void EmitWarning(CompilerWarningCode code, const Token& token, std::string_view message, std::optional<std::string_view> suggestion = std::nullopt);

private:
	const StaticAnalysisContext* m_context = nullptr;
	DiagnosticSink* m_sink = nullptr;
};
