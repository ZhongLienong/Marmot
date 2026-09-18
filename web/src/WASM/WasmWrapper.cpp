#ifdef __EMSCRIPTEN__

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/emscripten.h>
#include <sstream>
#include <string>
#include <iostream>
#include <exception>
#include <typeinfo>

#include "Compiler/Compiler.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"

using namespace std::string_literals;

struct ExecutionResult
{
	bool success;
	std::string output;
	std::string error;
	int exit_code;
};

namespace
{
	std::ostringstream g_output_stream;
	std::ostringstream g_error_stream;
	std::streambuf* g_original_cout_buffer = nullptr;
	std::streambuf* g_original_cerr_buffer = nullptr;

	// Strip ANSI color codes from string
	std::string StripAnsiCodes(const std::string& str)
	{
		std::string result;
		result.reserve(str.size());

		bool in_escape = false;
		for (size_t i = 0u; i < str.size(); i += 1u)
		{
			if (str[i] == '\033')
			{
				in_escape = true;
				continue;
			}

			if (in_escape)
			{
				if (str[i] == 'm')
				{
					in_escape = false;
				}
				continue;
			}

			result += str[i];
		}

		return result;
	}

	void StartCapture()
	{
		g_output_stream.str("");
		g_output_stream.clear();
		g_error_stream.str("");
		g_error_stream.clear();

		g_original_cout_buffer = std::cout.rdbuf(g_output_stream.rdbuf());
		g_original_cerr_buffer = std::cerr.rdbuf(g_error_stream.rdbuf());
	}

	void StopCapture()
	{
		if (g_original_cout_buffer != nullptr)
		{
			std::cout.rdbuf(g_original_cout_buffer);
		}
		if (g_original_cerr_buffer != nullptr)
		{
			std::cerr.rdbuf(g_original_cerr_buffer);
		}
	}
}

ExecutionResult ExecuteMarmotCode(const std::string& source_code)
{
	ExecutionResult result;
	result.success = false;
	result.exit_code = EXIT_FAILURE;

	StartCapture();

	MidoriResult::CompilerResult compile_result = Compiler(std::string(source_code), "/playground.mmt"s).Compile();

	if (!compile_result.has_value())
	{
		result.error = StripAnsiCodes(compile_result.error().Rendered());
		result.output = StripAnsiCodes(g_output_stream.str());
		StopCapture();
		return result;
	}

	try
	{
		MidoriExecutable executable = std::move(compile_result.value());
		VirtualMachine::ExecuteResult run_result = VirtualMachine(std::move(executable)).Execute();

		if (!run_result.has_value())
		{
			const RuntimeError& runtime_error = run_result.error();
			std::cout << runtime_error.Rendered();
			result.exit_code = runtime_error.ExitCode();
		}
		else
		{
			result.exit_code = run_result.value();
			result.success = (result.exit_code == EXIT_SUCCESS);
		}
	}
	catch (const std::exception& e)
	{
		result.success = false;
		result.exit_code = EXIT_FAILURE;
		result.error = std::string("Runtime error (") + typeid(e).name() + "): " + e.what();
	}
	catch (...)
	{
		result.success = false;
		result.exit_code = EXIT_FAILURE;
		result.error = "Unknown runtime error";
	}
	result.output = StripAnsiCodes(g_output_stream.str());
	std::string captured_error = StripAnsiCodes(g_error_stream.str());

	// If we have an exception message, append captured error to it
	if (!result.error.empty()) {
		if (!captured_error.empty()) {
			result.error += "\n" + captured_error;
		}
	} else {
		result.error = captured_error;
	}

	StopCapture();
	return result;
}

EMSCRIPTEN_BINDINGS(midori_module)
{
	emscripten::value_object<ExecutionResult>("ExecutionResult")
		.field("success", &ExecutionResult::success)
		.field("output", &ExecutionResult::output)
		.field("error", &ExecutionResult::error)
		.field("exitCode", &ExecutionResult::exit_code);

	emscripten::function("executeMarmotCode", &ExecuteMarmotCode);
}

#endif // __EMSCRIPTEN__
