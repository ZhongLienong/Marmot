#include <queue>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Compiler/CompilationSchedule/CompilationSchedule.h"

namespace
{
	void AddModule(BuildGraph& graph, const std::string& name, std::vector<std::string> dependencies, size_t tokens = 1u)
	{
		BuildGraph::BuildNode node;
		node.m_dependencies = std::move(dependencies);
		for (size_t index = 0u; index < tokens; index += 1u)
		{
			node.m_tokens.AddToken(Token("value", Token::Name::IDENTIFIER_LITERAL, 1, name));
		}
		graph.m_nodes.emplace(name, std::move(node));
	}

	void Enqueue(std::priority_queue<ScheduledModule>& queue, std::vector<ScheduledModule> modules)
	{
		for (ScheduledModule& module : modules)
		{
			queue.push(std::move(module));
		}
	}
}

TEST_CASE("Compilation prioritizes the longest downstream path", "[compiler][schedule]")
{
	BuildGraph graph;
	AddModule(graph, "AWide", {});
	AddModule(graph, "ZCritical", {});
	AddModule(graph, "Middle", { "ZCritical" }, 2u);
	AddModule(graph, "Main", { "AWide", "Middle" });
	CompilationSchedule schedule(graph);
	std::priority_queue<ScheduledModule> ready;
	Enqueue(ready, schedule.InitialReady());
	REQUIRE(ready.top().m_file_path == "ZCritical");
	ready.pop();

	// The importer becomes ready on the interface event while its dependency
	// is still optimizing; it also takes priority over the older wide work.
	Enqueue(ready, schedule.InterfaceReady("ZCritical"));
	REQUIRE(ready.top().m_file_path == "Middle");
	REQUIRE(ready.top().m_tier_idx == 1u);
	ready.pop();
	REQUIRE(schedule.InterfaceReady("Middle").empty());
	REQUIRE(ready.top().m_file_path == "AWide");
	const std::vector<ScheduledModule> main = schedule.InterfaceReady("AWide");
	REQUIRE(main.size() == 1u);
	REQUIRE(main.front().m_file_path == "Main");
}

TEST_CASE("Compilation priority accounts for source size", "[compiler][schedule]")
{
	BuildGraph graph;
	AddModule(graph, "Small", {}, 2u);
	AddModule(graph, "SmallImporter", { "Small" }, 2u);
	AddModule(graph, "Large", {}, 20u);
	AddModule(graph, "Main", { "SmallImporter", "Large" });
	CompilationSchedule schedule(graph);
	std::priority_queue<ScheduledModule> ready;
	Enqueue(ready, schedule.InitialReady());
	REQUIRE(ready.top().m_file_path == "Large");
}

TEST_CASE("Compilation releases a diamond once both interfaces are ready", "[compiler][schedule]")
{
	BuildGraph graph;
	AddModule(graph, "Shared", {});
	AddModule(graph, "Left", { "Shared" });
	AddModule(graph, "Right", { "Shared" });
	AddModule(graph, "Main", { "Left", "Right" });
	CompilationSchedule schedule(graph);
	std::priority_queue<ScheduledModule> ready;
	Enqueue(ready, schedule.InterfaceReady("Shared"));
	REQUIRE(ready.size() == 2u);
	REQUIRE(ready.top().m_file_path == "Left");
	REQUIRE(schedule.InterfaceReady("Right").empty());
	const std::vector<ScheduledModule> main = schedule.InterfaceReady("Left");
	REQUIRE(main.size() == 1u);
	REQUIRE(main.front().m_file_path == "Main");
	REQUIRE(schedule.m_tiers == std::vector<std::vector<std::string>>{ { "Shared" }, { "Left", "Right" }, { "Main" } });
}
