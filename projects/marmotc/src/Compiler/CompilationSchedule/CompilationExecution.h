#pragma once

#include "Compiler/CompilationSchedule/CompilationSchedule.h"
#include "Compiler/Result/Result.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace CompilationExecution
{
	using InterfaceReady = std::function<void(const std::string&)>;
	using CompileModule = std::function<MarmotResult::CompiledModuleReportResult(const ScheduledModule&, const InterfaceReady&)>;
	using Results = std::unordered_map<std::string, MarmotResult::CompiledModuleReportResult>;

	Results Run(CompilationSchedule& schedule, size_t worker_count, const CompileModule& compile);
}
