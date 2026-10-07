#include "BuildGraph.h"

#include <algorithm>
#include <map>
#include <queue>

namespace
{
	struct DependencyGraph
	{
		std::unordered_map<std::string, std::vector<std::string>> m_dependents;
		std::unordered_map<std::string, int> m_in_degrees;
	};

	DependencyGraph BuildDependencyGraph(const std::unordered_map<std::string, BuildGraph::BuildNode>& nodes)
	{
		DependencyGraph graph;

		for (const std::pair<const std::string, BuildGraph::BuildNode>& entry : nodes)
		{
			graph.m_dependents.emplace(entry.first, std::vector<std::string>{});
			graph.m_in_degrees.emplace(entry.first, 0);
		}

		for (const auto& [file, node] : nodes)
		{
			for (const std::string& dependency : node.m_dependencies)
			{
				if (!nodes.contains(dependency))
				{
					continue;
				}

				graph.m_dependents[dependency].emplace_back(file);
				graph.m_in_degrees[file] += 1;
			}
		}

		return graph;
	}

	std::unordered_map<std::string, int> ComputeLevels(const DependencyGraph& graph)
	{
		std::unordered_map<std::string, int> levels;
		std::unordered_map<std::string, int> remaining_in_degrees = graph.m_in_degrees;
		std::queue<std::string> zero_in_degree;

		for (const auto& [file, degree] : graph.m_in_degrees)
		{
			if (degree == 0)
			{
				zero_in_degree.push(file);
				levels.emplace(file, 0);
			}
		}

		while (!zero_in_degree.empty())
		{
			const std::string current = zero_in_degree.front();
			zero_in_degree.pop();

			const std::vector<std::string>& next_nodes = graph.m_dependents.at(current);
			for (const std::string& dependent : next_nodes)
			{
				remaining_in_degrees[dependent] -= 1;

				const int proposed_level = levels[current] + 1;
				std::unordered_map<std::string, int>::iterator level_it = levels.find(dependent);
				if (level_it == levels.end() || level_it->second < proposed_level)
				{
					levels[dependent] = proposed_level;
				}

				if (remaining_in_degrees[dependent] == 0)
				{
					zero_in_degree.push(dependent);
				}
			}
		}

		return levels;
	}

	std::map<int, std::vector<std::string>> GroupByLevel(const std::unordered_map<std::string, int>& levels)
	{
		std::map<int, std::vector<std::string>> groups;

		for (const auto& [path, level] : levels)
		{
			groups[level].emplace_back(path);
		}

		for (std::pair<const int, std::vector<std::string>>& group : groups)
		{
			std::vector<std::string>& tier = group.second;
			std::sort(tier.begin(), tier.end());
		}

		return groups;
	}

	std::vector<std::vector<std::string>> BuildTierList(std::map<int, std::vector<std::string>> groups)
	{
		std::vector<std::vector<std::string>> tiers;
		tiers.reserve(groups.size());

		for (std::pair<const int, std::vector<std::string>>& group : groups)
		{
			tiers.emplace_back(std::move(group.second));
		}

		return tiers;
	}
}

std::vector<std::vector<std::string>> BuildGraph::GetCompilationTiers() const
{
	return BuildTierList(GroupByLevel(ComputeLevels(BuildDependencyGraph(m_nodes))));
}
