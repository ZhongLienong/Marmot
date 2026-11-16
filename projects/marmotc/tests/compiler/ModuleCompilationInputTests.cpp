#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "Utility/Driver/MarmotDriver.h"
#include "support/TempProject.h"

TEST_CASE("Dependency conflicts keep the importing source and import order across jobs", "[compiler][module][imports]")
{
	const MarmotTest::TempProject project
	({
		MarmotTest::TempProjectFile("Alpha.mmt", R"(module Alpha
public export { Choice }
class Choice<T>
{
	choose: fn(value: T) -> Int;
};
)"),
		MarmotTest::TempProjectFile("Zulu.mmt", R"(module Zulu
public export { Choice }
class Choice<T>
{
	choose: fn(value: T) -> Text;
};
)"),
		MarmotTest::TempProjectFile("Main.mmt", R"(module Main
import
{
	"Zulu.mmt",
	"Alpha.mmt"
}
)")
	});
	const std::filesystem::path main = std::filesystem::weakly_canonical(project.Path("Main.mmt"));
	std::string expected;
	for (size_t jobs : { 1u, 2u, 4u, 2u })
	{
		const MarmotDriver::CompileFileWithReportResult result = MarmotDriver::CompileFileWithReport(main, CompilationInputs().WithJobs(jobs));
		REQUIRE_FALSE(result.has_value());
		const MarmotResult::CompilerReport& report = result.error().m_report;
		REQUIRE(report.m_errors.Size() == 1u);
		const CompilerError& error = report.m_errors.First();
		REQUIRE(error.m_location.has_value());
		CHECK(error.m_location->m_file_name == main.string());
		CHECK(error.m_location->m_line == 5);
		const std::string rendered = report.Rendered();
		CHECK(rendered.find("('Zulu' and 'Alpha')") != std::string::npos);
		if (expected.empty())
		{
			expected = rendered;
		}
		CHECK(rendered == expected);
	}
}
