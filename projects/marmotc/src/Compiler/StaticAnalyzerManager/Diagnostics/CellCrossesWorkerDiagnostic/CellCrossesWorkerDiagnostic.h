#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Warns where a Ref<T> is copied into another worker: a Spawn argument, a cell a
// function that crosses captures or reads as a global, a channel send, or a
// ParallelMap over cells. The copy is safe (each worker owns its cell afterwards)
// but easy to misread, since writes on one side are not seen on the other.
class CellCrossesWorkerDiagnostic final : public DiagnosticPass
{
public:
	static bool ContainsCell(const std::shared_ptr<MidoriType>& type);

protected:
	void Reset(MidoriProgramTree& program_tree) override;

	void operator()(MidoriExpression::Send& send) override;
	void operator()(MidoriExpression::Call& call) override;

private:
	static bool ContainsCell(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited);

	void WarnIfCell(const MidoriExpression& expression, const Token& token);

	// Warns about the cells a function value takes with it, when `value` is a
	// lambda or names one of this module's functions.
	void WarnIfFunctionCarriesCells(MidoriExpression& value, const Token& token);

	// This module's top-level functions, by name.
	std::unordered_map<std::string, MidoriExpression::Function*> m_module_functions;
};
