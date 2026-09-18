#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

#include <memory>
#include <unordered_set>

// Warns where a Cell<T> is copied into another worker: a Spawn argument, a cell
// the spawned lambda captures, a channel send, or a ParallelMap over cells. The
// copy is safe (each worker owns its cell afterwards) but easy to misread, since
// writes on one side are not seen on the other.
class CellCrossesWorkerDiagnostic final : public AstDiagnosticPass
{
public:
	std::string_view GetName() const override;

	static bool ContainsCell(const std::shared_ptr<MidoriType>& type);

protected:
	void operator()(MidoriExpression::Spawn& spawn) override;
	void operator()(MidoriExpression::Send& send) override;
	void operator()(MidoriExpression::Call& call) override;

private:
	static bool ContainsCell(const std::shared_ptr<MidoriType>& type, std::unordered_set<const MidoriType*>& visited);

	void WarnIfCell(const MidoriExpression& expression, const Token& token);
};
