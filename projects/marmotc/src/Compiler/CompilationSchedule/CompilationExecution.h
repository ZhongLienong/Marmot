#pragma once

#include "Compiler/CompilationSchedule/CompilationSchedule.h"
#include "Compiler/Module/CompiledModule.h"
#include "Compiler/Result/Result.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace MarmotResult
{
	using CompiledModuleReportResult = ReportResult<CompiledModule>;
}

namespace CompilationExecution
{
	using ExportsReady = std::function<void(const std::string&)>;
	using CompileModule = std::function<MarmotResult::CompiledModuleReportResult(const ScheduledModule&, const ExportsReady&)>;
	using Results = std::unordered_map<std::string, MarmotResult::CompiledModuleReportResult>;

	Results Run(CompilationSchedule& schedule, size_t worker_count, const CompileModule& compile);
}
