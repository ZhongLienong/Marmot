#pragma once

#include "Compiler/Result/Result.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Compiler/OptimizerManager/Optimizers/ConstantFolding/ConstantFolding.h"

class MidoriOptimizer;

#if MIDORI_ENABLE_OPTIMIZER_STATS
#include <mutex>
#include <string>

struct OptimizerLog
{
	bool m_enabled = false;
	std::string m_body;
};
#endif

class OptimizerManager
{
private:
	MidoriProgramTree m_program_tree;
	std::vector<std::unique_ptr<MidoriOptimizer>> m_optimizers;

#if MIDORI_ENABLE_OPTIMIZER_STATS
	struct OptimizerStats
	{
		std::string_view m_name;
		int m_optimizations_performed;
		int m_passes_run;
	};
	std::vector<OptimizerStats> m_stats;
#endif

public:
	static constexpr size_t s_max_iterations = 8u;

	OptimizerManager(MidoriProgramTree&& program_tree);

	void AddOptimizer(std::unique_ptr<MidoriOptimizer> optimizer);

#if MIDORI_ENABLE_OPTIMIZER_STATS
	MidoriResult::OptimizerResult Optimize(OptimizerLog* log = nullptr, std::mutex* print_mutex = nullptr);
#else
	MidoriResult::OptimizerResult Optimize();
#endif

#if MIDORI_ENABLE_OPTIMIZER_STATS
	void PrintStatistics() const;
#endif
};
