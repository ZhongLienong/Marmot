#include "Compiler/PackageManager/PackageResolver.h"

#include <algorithm>
#include <format>
#include <functional>
#include <queue>
#include <ranges>

namespace
{
	[[nodiscard]] std::string FormatDependencyChain(const std::vector<std::string>& stack, std::string_view package_name)
	{
		std::string chain;
		for (const std::string& entry : stack)
		{
			if (!chain.empty())
			{
				chain.append(" -> ");
			}
			chain.append(entry);
		}

		if (!chain.empty())
		{
			chain.append(" -> ");
		}
		chain.append(package_name);
		return chain;
	}
}

namespace MidoriPackageManager
{
	const ResolvedPackage* ResolvedPackageGraph::Find(std::string_view package_name) const
	{
		const std::unordered_map<std::string, ResolvedPackage>::const_iterator entry = m_packages.find(std::string(package_name));
		if (entry == m_packages.end())
		{
			return nullptr;
		}
		return &entry->second;
	}

	std::vector<std::string> ResolvedPackageGraph::TopologicalOrder() const
	{
		std::unordered_map<std::string, int> in_degree;
		std::unordered_map<std::string, std::vector<std::string>> dependents;
		for (const auto& [package_name, _] : m_packages)
		{
			in_degree[package_name] = 0;
			dependents[package_name] = {};
		}

		for (const auto& [package_name, resolved_package] : m_packages)
		{
			for (const std::string& dependency : resolved_package.m_dependencies)
			{
				if (!m_packages.contains(dependency))
				{
					continue;
				}

				dependents[dependency].push_back(package_name);
				in_degree[package_name] += 1;
			}
		}

		std::priority_queue<std::string, std::vector<std::string>, std::greater<>> ready;
		for (const auto& [package_name, degree] : in_degree)
		{
			if (degree == 0)
			{
				ready.push(package_name);
			}
		}

		std::vector<std::string> order;
		order.reserve(m_packages.size());
		while (!ready.empty())
		{
			const std::string package_name = ready.top();
			ready.pop();
			order.push_back(package_name);

			for (const std::string& dependent : dependents[package_name])
			{
				in_degree[dependent] -= 1;
				if (in_degree[dependent] == 0)
				{
					ready.push(dependent);
				}
			}
		}

		return order;
	}

	std::vector<std::filesystem::path> ResolvedPackageGraph::PackageSearchPaths() const
	{
		std::vector<std::filesystem::path> paths;
		const std::vector<std::string> order = TopologicalOrder();
		paths.reserve(order.size());
		for (const std::string& package_name : order)
		{
			paths.push_back(m_packages.at(package_name).m_manifest.GetPackageDirectory());
		}
		return paths;
	}

	PackageResolver::PackageResolver(const PackageIndex& index)
		: m_index(index)
	{
	}

	std::expected<ResolvedPackageGraph, std::string> PackageResolver::Resolve(
		const std::unordered_map<std::string, MidoriVersion::VersionConstraint>& root_dependencies) const
	{
		ResolvedPackageGraph graph;
		graph.m_root_dependencies.reserve(root_dependencies.size());
		for (const auto& [package_name, _] : root_dependencies)
		{
			graph.m_root_dependencies.push_back(package_name);
		}
		std::ranges::sort(graph.m_root_dependencies);

		std::vector<std::string> stack;
		for (const std::string& package_name : graph.m_root_dependencies)
		{
			const std::expected<void, std::string> result = ResolvePackage(
				graph,
				package_name,
				root_dependencies.at(package_name),
				"<root>",
				stack);
			if (!result.has_value())
			{
				return std::unexpected(result.error());
			}
		}

		return graph;
	}

	std::expected<void, std::string> PackageResolver::ResolvePackage(
		ResolvedPackageGraph& graph,
		const std::string& package_name,
		const MidoriVersion::VersionConstraint& constraint,
		const std::string& required_by,
		std::vector<std::string>& stack) const
	{
		if (std::ranges::find(stack, package_name) != stack.end())
		{
			return std::unexpected(std::format(
				"Detected a package dependency cycle: {}",
				FormatDependencyChain(stack, package_name)));
		}

		if (const ResolvedPackage* existing = graph.Find(package_name))
		{
			if (!constraint.Matches(existing->m_manifest.GetInfo().m_semantic_version))
			{
				return std::unexpected(std::format(
					"Version conflict for package '{}': {} requires '{}', but the resolved version is {}.",
					package_name,
					required_by,
					constraint.ToString(),
					existing->m_manifest.GetInfo().m_version));
			}

			return {};
		}

		const std::vector<PackageIndexEntry> matches = m_index.FindPackagesMatching(package_name, constraint);
		if (matches.empty())
		{
			return std::unexpected(std::format(
				"Could not resolve package '{}' required by {} with constraint '{}'.",
				package_name,
				required_by,
				constraint.ToString()));
		}

		const PackageManifest& manifest = matches.front().m_manifest;
		ResolvedPackage resolved_package;
		resolved_package.m_manifest = manifest;
		for (const auto& [dependency_name, _] : manifest.GetDependencies().m_constraints)
		{
			resolved_package.m_dependencies.push_back(dependency_name);
		}
		std::ranges::sort(resolved_package.m_dependencies);
		graph.m_packages[package_name] = std::move(resolved_package);

		stack.push_back(package_name);
		for (const std::string& dependency_name : graph.m_packages.at(package_name).m_dependencies)
		{
			const MidoriVersion::VersionConstraint& dependency_constraint = manifest.GetDependencies().m_constraints.at(dependency_name);
			const std::expected<void, std::string> result = ResolvePackage(
				graph,
				dependency_name,
				dependency_constraint,
				package_name,
				stack);
			if (!result.has_value())
			{
				graph.m_packages.erase(package_name);
				stack.pop_back();
				return result;
			}
		}
		stack.pop_back();

		return {};
	}
}
