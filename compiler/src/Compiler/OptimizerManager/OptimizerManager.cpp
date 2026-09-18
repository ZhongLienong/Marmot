#include "OptimizerManager.h"
#include "Optimizers/BaseOptimizer/BaseOptimizer.h"
#include "Compiler/OptimizerManager/Optimizers/ConstantFolding/ConstantFolding.h"
#include "Compiler/OptimizerManager/Optimizers/FunctionInlining/FunctionInlining.h"
#include "Compiler/OptimizerManager/Optimizers/StrengthReduction/StrengthReduction.h"
#include "Compiler/OptimizerManager/Optimizers/ConstantBranchElimination/ConstantBranchElimination.h"
#include "Compiler/OptimizerManager/Optimizers/LocalConstantPropagation/LocalConstantPropagation.h"
#include "Compiler/OptimizerManager/Optimizers/DeadCodeElimination/DeadCodeElimination.h"
#include "Compiler/OptimizerManager/Optimizers/CanonicalizationCleanup/CanonicalizationCleanup.h"
#include "Compiler/OptimizerManager/Optimizers/ClosureLifting/ClosureLifting.h"
#include "Compiler/OptimizerManager/Optimizers/TailCallOptimization/TailCallOptimization.h"
#include "Common/BuildConfig/BuildConfig.h"

#if MIDORI_ENABLE_OPTIMIZER_STATS
#include "Common/Printer/Printer.h"
#include <format>
#include <iterator>
#include <optional>
#endif

OptimizerManager::OptimizerManager(MidoriProgramTree&& program_tree)
	: m_program_tree(std::move(program_tree))
{
	AddOptimizer(std::make_unique<ConstantFolding>());
	AddOptimizer(std::make_unique<FunctionInlining>());
	AddOptimizer(std::make_unique<StrengthReduction>());
	AddOptimizer(std::make_unique<ConstantBranchElimination>());
	AddOptimizer(std::make_unique<LocalConstantPropagation>());
	AddOptimizer(std::make_unique<DeadCodeElimination>());
	AddOptimizer(std::make_unique<CanonicalizationCleanup>());
	AddOptimizer(std::make_unique<ClosureLifting>());
	AddOptimizer(std::make_unique<TailCallOptimization>());
}

void OptimizerManager::AddOptimizer(std::unique_ptr<MidoriOptimizer> optimizer)
{
	m_optimizers.emplace_back(std::move(optimizer));

#if MIDORI_ENABLE_OPTIMIZER_STATS
	m_stats.emplace_back("", 0, 0);
#endif
}

#if MIDORI_ENABLE_OPTIMIZER_STATS
MidoriResult::OptimizerResult OptimizerManager::Optimize(OptimizerLog* log, std::mutex* print_mutex)
{
	std::string optimization_body;
	optimization_body.reserve(m_optimizers.size() * s_max_iterations * 48u);

	MidoriResult::OptimizerResult result = std::move(m_program_tree);
	for (size_t iteration = 0u; iteration < s_max_iterations && result.has_value(); iteration += 1u)
	{
		bool iteration_changed = false;
		std::string iteration_body;
		iteration_body.reserve(m_optimizers.size() * 40u);

		for (size_t optimizer_index = 0u; optimizer_index < m_optimizers.size(); optimizer_index += 1u)
		{
			MidoriOptimizer& optimizer = *m_optimizers[optimizer_index];
			OptimizerStats& stat = m_stats[optimizer_index];
			const std::string_view name = optimizer.GetName();
			stat.m_name = name;
			stat.m_passes_run += 1;

			MidoriProgramTree program_tree = std::move(result).value();
			result = optimizer.Optimize(std::move(program_tree));

			const int optimizations = optimizer.GetOptimizationsPerformed();
			stat.m_optimizations_performed += optimizations;
			iteration_changed = iteration_changed || optimizer.DidChange();

			if (optimizations > 0)
			{
				if (iteration_body.empty())
				{
					std::format_to(std::back_inserter(iteration_body), "Iteration {}:\n", iteration + 1u);
				}

				std::format_to(std::back_inserter(iteration_body), "  {}: {} optimizations\n", name, optimizations);
			}

			if (!result.has_value())
			{
				break;
			}
		}

		optimization_body += iteration_body;

		if (!iteration_changed)
		{
			break;
		}
	}

	const auto print_output = [&optimization_body]()
	{
		Printer::Print<Printer::Color::CYAN>("\n=== Optimization Pass ===\n");
		if (!optimization_body.empty())
		{
			Printer::Print<Printer::Color::MAGENTA>(optimization_body);
		}
		Printer::Print<Printer::Color::CYAN>("=========================\n\n");
	};

	std::optional<OptimizerLog*> log_target = log ? std::optional<OptimizerLog*>{ log } : std::nullopt;
	log_target
		.and_then
		(
			[&optimization_body](OptimizerLog* log_ptr) -> std::optional<bool>
			{
				log_ptr->m_enabled = true;
				log_ptr->m_body = std::move(optimization_body);
				return true;
			}
		)
		.or_else
		(
			[&print_mutex, &print_output]()
			{
				if (print_mutex)
				{
					std::lock_guard<std::mutex> lock(*print_mutex);
					print_output();
				}
				else
				{
					print_output();
				}
				return std::optional<bool>{ true };
			}
		);

	return std::move(result);
}
#else
MidoriResult::OptimizerResult OptimizerManager::Optimize()
{
	MidoriResult::OptimizerResult result = std::move(m_program_tree);

	for (size_t iteration = 0u; iteration < s_max_iterations && result.has_value(); iteration += 1u)
	{
		bool iteration_changed = false;

		for (const std::unique_ptr<MidoriOptimizer>& optimizer : m_optimizers)
		{
			MidoriProgramTree program_tree = std::move(result).value();
			result = optimizer->Optimize(std::move(program_tree));
			iteration_changed = iteration_changed || optimizer->DidChange();

			if (!result.has_value())
			{
				break;
			}
		}

		if (!iteration_changed)
		{
			break;
		}
	}

	return result;
}
#endif

#if MIDORI_ENABLE_OPTIMIZER_STATS
void OptimizerManager::PrintStatistics() const
{
	Printer::Print<Printer::Color::CYAN>("\n=== Optimization Statistics ===\n");

	int total_optimizations = 0;
	for (const OptimizerManager::OptimizerStats& stat : m_stats)
	{
		if (stat.m_optimizations_performed > 0)
		{
			Printer::Print<Printer::Color::GREEN>(std::format("  {:20}: {:4} optimizations ({} passes)\n", stat.m_name, stat.m_optimizations_performed, stat.m_passes_run));
			total_optimizations += stat.m_optimizations_performed;
		}
	}

	Printer::Print<Printer::Color::YELLOW>(std::format("  {:20}: {:4} total optimizations\n", "TOTAL", total_optimizations));
	Printer::Print<Printer::Color::CYAN>("================================\n\n");
}
#endif
