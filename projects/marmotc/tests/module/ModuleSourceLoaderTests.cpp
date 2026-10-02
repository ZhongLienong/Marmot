#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "Compiler/ModuleManager/ModuleSourceLoader.h"

TEST_CASE("Source prefetch overlaps reads within the shared job budget", "[module][discovery]")
{
	std::atomic<size_t> active{ 0u };
	std::atomic<size_t> peak{ 0u };
	std::atomic<size_t> started{ 0u };
	std::promise<void> second_started;
	const std::shared_future<void> both = second_started.get_future().share();
	ModuleSourceLoader loader(2u, [&](const std::string& file_path) -> ModuleSourceLoader::LoadResult
	{
		const size_t current = active.fetch_add(1u) + 1u;
		size_t previous = peak.load();
		while (previous < current && !peak.compare_exchange_weak(previous, current))
		{
		}
		const size_t index = started.fetch_add(1u) + 1u;
		if (index == 2u)
		{
			second_started.set_value();
		}
		const bool overlapped = both.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
		active.fetch_sub(1u);
		if (!overlapped)
		{
			return std::unexpected(CompilerError::Simple(CompilerStage::Compiler, "Independent reads did not overlap."));
		}
		return ImportedSource(TokenStream{}, { file_path });
	});
	loader.Prefetch({ "First", "Second", "Third", "First" });
	loader.Prefetch({ "First", "Second" });
	for (const std::string& file_path : { "First", "Second", "Third" })
	{
		MidoriResult::Result<ImportedSource> source = loader.Take(file_path, "Main", 2);
		REQUIRE(source.has_value());
		REQUIRE(source->m_source_lines == std::vector<std::string>{ file_path });
	}
	REQUIRE(peak.load() == 2u);
	REQUIRE(started.load() == 3u);
}

TEST_CASE("One discovery job reads on the calling thread", "[module][discovery]")
{
	const std::thread::id caller = std::this_thread::get_id();
	std::thread::id reader;
	ModuleSourceLoader loader(1u, [&](const std::string& file_path) -> ModuleSourceLoader::LoadResult
	{
		reader = std::this_thread::get_id();
		return ImportedSource(TokenStream{}, { file_path });
	});
	loader.Prefetch({ "First", "Second" });
	REQUIRE(loader.Take("First", "Main", 2).has_value());
	REQUIRE(reader == caller);
	REQUIRE(loader.Take("Second", "Main", 2).has_value());
	REQUIRE(reader == caller);
}

TEST_CASE("Prefetched open errors name the importer that consumes the file", "[module][discovery]")
{
	ModuleSourceLoader loader(2u, [](const std::string&) -> ModuleSourceLoader::LoadResult
	{
		return std::unexpected(ModuleSourceLoader::OpenFailure::File);
	});
	loader.Prefetch({ "Directory.mmt", "Other.mmt" });
	MidoriResult::Result<ImportedSource> source = loader.Take("Directory.mmt", "Nested.mmt", 7);
	REQUIRE(!source.has_value());
	REQUIRE(source.error().m_code == CompilerErrorCode::ModuleImportFileOpenFailed);
	REQUIRE(source.error().m_location.has_value());
	REQUIRE(source.error().m_location->m_file_name == "Nested.mmt");
	REQUIRE(source.error().m_location->m_line == 7);
}
