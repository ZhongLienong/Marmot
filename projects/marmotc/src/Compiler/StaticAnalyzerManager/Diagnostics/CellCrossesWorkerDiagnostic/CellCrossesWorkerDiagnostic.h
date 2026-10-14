#pragma once

#include "Compiler/Analysis/WorkerCrossing.h"
#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Warns where a Ref<T> is copied between workers: an argument a call hands to a
// worker, by Spawn or by a function whose summary says it reaches one; a cell a
// function that crosses captures or reads as a global; a channel send; and a
// result that comes back from a worker. The copy is safe (each worker owns its
// cell afterwards) but easy to misread, since writes on one side are not seen on
// the other.
class CellCrossesWorkerDiagnostic final : public DiagnosticPass
{
public:
	explicit CellCrossesWorkerDiagnostic(const WorkerCrossings& crossings);

protected:
	void Reset(MidoriProgramTree& program_tree) override;

	void operator()(MidoriExpression::Send& send) override;
	void operator()(MidoriExpression::Call& call) override;

private:
	void WarnIfCell(const MidoriExpression& expression, const Token& token);

	// Warns about the cells a function value takes with it, when `value` is a
	// lambda or names one of this module's functions.
	void WarnIfFunctionCarriesCells(MidoriExpression& value, const Token& token);

	// This module's functions' summaries and those of what it imports.
	const WorkerCrossings& m_crossings;
	// This module's top-level functions, by name.
	std::unordered_map<std::string, MidoriExpression::Function*> m_module_functions;
};
