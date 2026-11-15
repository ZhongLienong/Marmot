#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

class IntegerOverflowDiagnostic final : public DiagnosticPass
{
protected:
	void operator()(MarmotExpression::Binary& binary) override;
};
