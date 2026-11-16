#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "Compiler/CompilationSchedule/CompilationExecution.h"

namespace
{
	void AddModule(BuildGraph& graph, const std::string& name, std::vector<std::string> dependencies = {})
	{
		BuildGraph::BuildNode node;
		node.m_dependencies = std::move(dependencies);
		graph.m_nodes.emplace(name, std::move(node));
	}

	MarmotResult::CompiledModuleReportResult Success(const std::string& name)
	{
		return CompiledModule(std::make_shared<const ModuleInterface>(ModuleInterface(name, name, {}, {}, {}, {}, {})));
	}

	MarmotResult::CompiledModuleReportResult Failure(const std::string& message)
	{
		return std::unexpected(MarmotResult::CompilerReport(CompilerError::Simple(CompilerStage::Compiler, message)));
	}
}

TEST_CASE("One compilation worker runs dependency work on the caller", "[compiler][execution]")
{
	BuildGraph graph;
	AddModule(graph, "Shared");
	AddModule(graph, "Main", { "Shared" });
	CompilationSchedule schedule(graph);
	const std::thread::id caller = std::this_thread::get_id();
	bool on_caller = true;
	std::vector<std::string> started;
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 1u, [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady& publish)
	{
		on_caller = on_caller && std::this_thread::get_id() == caller;
		started.push_back(module.m_file_path);
		publish(module.m_file_path);
		return Success(module.m_file_path);
	});
	CHECK(on_caller);
	CHECK(started == std::vector<std::string>{ "Shared", "Main" });
	REQUIRE(results.size() == 2u);
	CHECK(results.at("Shared").has_value());
	CHECK(results.at("Main").has_value());
}

TEST_CASE("An empty compilation schedule finishes without starting work", "[compiler][execution]")
{
	BuildGraph graph;
	CompilationSchedule schedule(graph);
	bool started = false;
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 0u, [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady&)
	{
		started = true;
		return Success(module.m_file_path);
	});
	CHECK_FALSE(started);
	CHECK(results.empty());
}

TEST_CASE("Compilation workers overlap within the job budget", "[compiler][execution]")
{
	BuildGraph graph;
	for (const std::string& name : { "A", "B", "C", "D", "E", "F" })
	{
		AddModule(graph, name);
	}
	CompilationSchedule schedule(graph);
	std::atomic<size_t> active{ 0u };
	std::atomic<size_t> peak{ 0u };
	std::atomic<size_t> started{ 0u };
	std::promise<void> second_started;
	const std::shared_future<void> both = second_started.get_future().share();
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 2u, [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady&)
	{
		const size_t current = active.fetch_add(1u) + 1u;
		size_t previous = peak.load();
		while (previous < current && !peak.compare_exchange_weak(previous, current))
		{
		}
		if (started.fetch_add(1u) + 1u == 2u)
		{
			second_started.set_value();
		}
		const bool overlapped = both.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
		active.fetch_sub(1u);
		return overlapped ? Success(module.m_file_path) : Failure("Independent compilations did not overlap.");
	});
	REQUIRE(results.size() == 6u);
	for (const auto& [name, result] : results)
	{
		CAPTURE(name);
		CHECK(result.has_value());
	}
	CHECK(peak.load() == 2u);
	CHECK(started.load() == 6u);
}

TEST_CASE("An importer overlaps its dependency backend even when that backend fails", "[compiler][execution]")
{
	BuildGraph graph;
	AddModule(graph, "Dependency");
	AddModule(graph, "Importer", { "Dependency" });
	CompilationSchedule schedule(graph);
	std::promise<void> importer_started;
	const std::future<void> importer = importer_started.get_future();
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 2u, [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady& publish)
	{
		if (module.m_file_path == "Dependency")
		{
			publish(module.m_file_path);
			if (importer.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			{
				return Failure("Importer waited for dependency completion.");
			}
			return Failure("Backend failure after publication.");
		}
		importer_started.set_value();
		publish(module.m_file_path);
		return Success(module.m_file_path);
	});
	REQUIRE(results.size() == 2u);
	REQUIRE_FALSE(results.at("Dependency").has_value());
	CHECK(results.at("Dependency").error().Rendered().find("Backend failure after publication.") != std::string::npos);
	CHECK(results.at("Importer").has_value());
}

TEST_CASE("An unpublished failure blocks its importers while independent work finishes", "[compiler][execution]")
{
	BuildGraph graph;
	AddModule(graph, "Broken");
	AddModule(graph, "Blocked", { "Broken" });
	AddModule(graph, "Independent");
	AddModule(graph, "IndependentImporter", { "Independent" });
	AddModule(graph, "Main", { "Blocked", "IndependentImporter" });
	CompilationSchedule schedule(graph);
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 4u, [](const ScheduledModule& module, const CompilationExecution::InterfaceReady& publish)
	{
		if (module.m_file_path == "Broken")
		{
			return Failure("Failure before publication.");
		}
		publish(module.m_file_path);
		return Success(module.m_file_path);
	});
	REQUIRE(results.size() == 3u);
	CHECK_FALSE(results.at("Broken").has_value());
	CHECK(results.at("Independent").has_value());
	CHECK(results.at("IndependentImporter").has_value());
	CHECK_FALSE(results.contains("Blocked"));
	CHECK_FALSE(results.contains("Main"));
}

TEST_CASE("Concurrent diamond publications release the importer once", "[compiler][execution]")
{
	BuildGraph graph;
	AddModule(graph, "Left");
	AddModule(graph, "Right");
	AddModule(graph, "Main", { "Left", "Right" });
	CompilationSchedule schedule(graph);
	std::atomic<size_t> siblings{ 0u };
	std::atomic<size_t> publications{ 0u };
	std::atomic<size_t> importer_runs{ 0u };
	std::promise<void> second_started;
	const std::shared_future<void> both = second_started.get_future().share();
	const CompilationExecution::Results results = CompilationExecution::Run(schedule, 2u, [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady& publish)
	{
		if (module.m_file_path == "Main")
		{
			importer_runs.fetch_add(1u);
			return publications.load() == 2u ? Success(module.m_file_path) : Failure("Importer ran before both publications.");
		}
		if (siblings.fetch_add(1u) + 1u == 2u)
		{
			second_started.set_value();
		}
		if (both.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
		{
			return Failure("Sibling compilations did not overlap.");
		}
		publications.fetch_add(1u);
		publish(module.m_file_path);
		return Success(module.m_file_path);
	});
	REQUIRE(results.size() == 3u);
	for (const auto& [name, result] : results)
	{
		CAPTURE(name);
		CHECK(result.has_value());
	}
	CHECK(importer_runs.load() == 1u);
}

TEST_CASE("Unwinding compilation execution stops waiting workers", "[compiler][execution]")
{
	BuildGraph graph;
	AddModule(graph, "A");
	AddModule(graph, "B");
	CompilationSchedule schedule(graph);
	const std::thread::id caller = std::this_thread::get_id();
	std::promise<void> background_started;
	const std::shared_future<void> background = background_started.get_future().share();
	std::promise<void> caller_started;
	const std::shared_future<void> foreground = caller_started.get_future().share();
	const CompilationExecution::CompileModule compile = [&](const ScheduledModule& module, const CompilationExecution::InterfaceReady&) -> MarmotResult::CompiledModuleReportResult
	{
		if (std::this_thread::get_id() == caller)
		{
			caller_started.set_value();
			if (background.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
			{
				return Failure("Background worker did not start.");
			}
			throw std::runtime_error("caller failure");
		}
		background_started.set_value();
		if (foreground.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
		{
			return Failure("Calling thread did not join compilation work.");
		}
		return Success(module.m_file_path);
	};
	REQUIRE_THROWS_WITH(CompilationExecution::Run(schedule, 2u, compile), "caller failure");
}
