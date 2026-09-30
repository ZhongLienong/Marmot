#pragma once

#include "Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.h"

class IntegerOverflowDiagnostic final : public AstDiagnosticPass
{
public:
	std::string_view GetName() const override;

protected:
	void operator()(MidoriExpression::Binary& binary) override;
};
