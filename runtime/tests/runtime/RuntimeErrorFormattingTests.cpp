#include <catch2/catch_test_macros.hpp>

#include "Error/RuntimeError.h"

#include <string>
#include <string_view>
#include <vector>

namespace
{
	std::string StripAnsiCodes(std::string_view text)
	{
		std::string stripped;
		stripped.reserve(text.size());

		bool in_escape = false;
		for (const char ch : text)
		{
			if (ch == '\033')
			{
				in_escape = true;
				continue;
			}

			if (in_escape)
			{
				if (ch == 'm')
				{
					in_escape = false;
				}
				continue;
			}

			stripped.push_back(ch);
		}

		return stripped;
	}
}

TEST_CASE("Runtime errors render file-backed source lines", "[error][format][runtime]")
{
	const RuntimeError runtime_error =
		RuntimeError(
			RuntimeErrorCode::IndexOutOfBounds,
			"Index out of bounds at index: 4.",
			RuntimeErrorLocation
			{
				.m_file_name = "Runtime.mmt",
				.m_line = 2,
				.m_source_line = "def value = [1, 2][4];"
			});
	const std::string rendered = StripAnsiCodes(runtime_error.Rendered());

	const std::string expected_render =
		"error[IndexOutOfBounds]: Index out of bounds at index: 4.\n"
		" --> Runtime.mmt:2\n"
		"  |\n"
		"2 | def value = [1, 2][4];\n"
		"  | Index out of bounds at index: 4.\n"
		"  |\n";

	REQUIRE(rendered == expected_render);
	REQUIRE(runtime_error.ExitCode() == 1);
}

TEST_CASE("Machine-readable runtime errors serialize runtime code and stack metadata", "[runtime][error][json]")
{
	RuntimeError runtime_error = RuntimeError(
		RuntimeErrorCode::StackOverflow,
		"Stack overflow - exceeded maximum call depth.",
		RuntimeErrorLocation
		{
			.m_file_name = "Runtime.mmt",
			.m_line = 5,
			.m_source_line = "def value = recurse(0);"
		},
		std::vector<RuntimeStackFrame>
		{
			RuntimeStackFrame
			{
				.m_procedure_name = "recurse",
				.m_module_name = "Runtime",
				.m_location = RuntimeErrorLocation
				{
					.m_file_name = "Runtime.mmt",
					.m_line = 2,
					.m_source_line = "def recurse = fn(n : Int) -> Int => recurse(n + 1) + 1;"
				},
				.m_recursive_call_count = 12
			}
		});

	const std::string serialized = SerializeMachineReadableRuntimeError(runtime_error);
	CHECK(serialized.find("\"source\":\"marmot-runtime\"") != std::string::npos);
	CHECK(serialized.find("\"code\":\"StackOverflow\"") != std::string::npos);
	CHECK(serialized.find("\"kind\":\"panic\"") != std::string::npos);
	CHECK(serialized.find("\"exitCode\":2") != std::string::npos);
	CHECK(serialized.find("\"stack\":[{") != std::string::npos);
	CHECK(serialized.find("\"procedure\":\"recurse\"") != std::string::npos);
	CHECK(serialized.find("\"recursiveCount\":12") != std::string::npos);
}
