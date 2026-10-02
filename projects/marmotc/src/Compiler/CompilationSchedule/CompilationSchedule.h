#pragma once

#include "Compiler/BuildGraph/BuildGraph.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ScheduledModule
{
	std::string m_file_path;
	size_t m_tier_idx;
	size_t m_priority;

	ScheduledModule(std::string file_path, size_t tier_idx, size_t priority);
	bool operator<(const ScheduledModule& other) const;
};

struct CompilationSchedule
{
	std::vector<std::vector<std::string>> m_tiers;
	std::vector<std::string> m_all_modules;
	std::unordered_map<std::string, size_t> m_tier_indices;
	std::unordered_map<std::string, size_t> m_remaining_deps;
	std::unordered_map<std::string, std::vector<std::string>> m_dependents;
	std::unordered_map<std::string, size_t> m_priorities;

	explicit CompilationSchedule(const BuildGraph& build_graph);
	std::vector<ScheduledModule> InitialReady() const;
	std::vector<ScheduledModule> InterfaceReady(const std::string& file_path);

private:
	ScheduledModule Schedule(const std::string& file_path) const;
};
