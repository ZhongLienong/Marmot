#include "CompilationSchedule.h"

#include <algorithm>
#include <ranges>
#include <utility>

ScheduledModule::ScheduledModule(std::string file_path, size_t tier_idx, size_t priority)
	: m_file_path(std::move(file_path)),
	m_tier_idx(tier_idx),
	m_priority(priority)
{
}

bool ScheduledModule::operator<(const ScheduledModule& other) const
{
	if (m_priority != other.m_priority)
	{
		return m_priority < other.m_priority;
	}
	return m_file_path > other.m_file_path;
}

CompilationSchedule::CompilationSchedule(const BuildGraph& build_graph)
	: m_tiers(build_graph.GetCompilationTiers())
{
	m_all_modules.reserve(build_graph.m_nodes.size());
	for (const auto& [file_path, node] : build_graph.m_nodes)
	{
		m_all_modules.push_back(file_path);
		m_remaining_deps.emplace(file_path, node.m_dependencies.size());
		m_dependents.emplace(file_path, std::vector<std::string>{});
	}
	std::ranges::sort(m_all_modules);

	for (size_t tier_idx = 0u; tier_idx < m_tiers.size(); tier_idx += 1u)
	{
		for (const std::string& file_path : m_tiers[tier_idx])
		{
			m_tier_indices.emplace(file_path, tier_idx);
		}
	}

	for (const std::string& file_path : m_all_modules)
	{
		for (const std::string& dependency : build_graph.m_nodes.at(file_path).m_dependencies)
		{
			m_dependents.at(dependency).push_back(file_path);
		}
	}

	// Token count estimates work; the longest downstream path identifies the
	// modules whose delay would keep the rest of the build waiting.
	for (const std::vector<std::string>& tier : m_tiers | std::views::reverse)
	{
		for (const std::string& file_path : tier)
		{
			size_t downstream = 0u;
			for (const std::string& dependent : m_dependents.at(file_path))
			{
				downstream = std::max(downstream, m_priorities.at(dependent));
			}
			m_priorities.emplace(file_path, downstream + std::max(size_t{ 1u }, static_cast<size_t>(build_graph.m_nodes.at(file_path).m_tokens.Size())));
		}
	}
}

ScheduledModule CompilationSchedule::Schedule(const std::string& file_path) const
{
	return ScheduledModule(file_path, m_tier_indices.at(file_path), m_priorities.at(file_path));
}

std::vector<ScheduledModule> CompilationSchedule::InitialReady() const
{
	std::vector<ScheduledModule> ready;
	for (const std::string& file_path : m_all_modules)
	{
		if (m_remaining_deps.at(file_path) == 0u)
		{
			ready.push_back(Schedule(file_path));
		}
	}
	return ready;
}

std::vector<ScheduledModule> CompilationSchedule::InterfaceReady(const std::string& file_path)
{
	std::vector<ScheduledModule> ready;
	for (const std::string& dependent : m_dependents.at(file_path))
	{
		size_t& remaining = m_remaining_deps.at(dependent);
		remaining -= 1u;
		if (remaining == 0u)
		{
			ready.push_back(Schedule(dependent));
		}
	}
	return ready;
}
