#include <barrier>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "Compiler/Module/ModuleExportMetadata.h"
#include "Compiler/Module/ModuleExportMetadataRegistry.h"

namespace
{
	std::shared_ptr<const ModuleExportMetadata> MakeExportMetadata(const std::string& name)
	{
		return std::make_shared<const ModuleExportMetadata>(ModuleExportMetadata(name, name + ".mmt", {}, {}, {}, {}));
	}
}

TEST_CASE("Dependency snapshots preserve import order and own the export metadata", "[module][publication]")
{
	std::vector<std::shared_ptr<const ModuleExportMetadata>> dependencies;
	{
		ModuleExportMetadataRegistry registry(2u);
		registry.Publish("Alpha.mmt", MakeExportMetadata("Alpha"));
		registry.Publish("Zulu.mmt", MakeExportMetadata("Zulu"));
		dependencies = registry.Snapshot(std::vector<std::string>{ "Zulu.mmt", "Alpha.mmt" });
	}
	REQUIRE(dependencies.size() == 2u);
	CHECK(dependencies[0]->m_module_name == "Zulu");
	CHECK(dependencies[1]->m_module_name == "Alpha");
}

TEST_CASE("Dependency snapshots stay stable during independent publications", "[module][publication]")
{
	ModuleExportMetadataRegistry registry(129u);
	registry.Publish("Origin.mmt", MakeExportMetadata("Origin"));
	const std::vector<std::string> imports{ "Origin.mmt" };
	const std::vector<std::shared_ptr<const ModuleExportMetadata>> original = registry.Snapshot(imports);
	std::barrier phase(2);
	bool stable = true;
	{
		std::jthread publisher
		(
			[&]()
			{
				for (size_t index = 0u; index < 128u; index += 1u)
				{
					phase.arrive_and_wait();
					const std::string name = "Sibling" + std::to_string(index);
					registry.Publish(name + ".mmt", MakeExportMetadata(name));
				}
			}
		);
		for (size_t index = 0u; index < 128u; index += 1u)
		{
			phase.arrive_and_wait();
			const std::vector<std::shared_ptr<const ModuleExportMetadata>> dependencies = registry.Snapshot(imports);
			stable = stable && dependencies == original;
		}
	}
	CHECK(stable);
	CHECK(original.front()->m_module_name == "Origin");
}
