#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Loader/ProgramLoader.h"
#include "Bytecode/Artifact/BinaryArtifact.h"

namespace
{
	class StreamCapture
	{
	private:
		std::ostringstream m_stdout;
		std::ostringstream m_stderr;
		std::streambuf* m_previous_stdout;
		std::streambuf* m_previous_stderr;

	public:
		StreamCapture()
			: m_previous_stdout(std::cout.rdbuf(m_stdout.rdbuf())),
			m_previous_stderr(std::cerr.rdbuf(m_stderr.rdbuf()))
		{
		}

		~StreamCapture()
		{
			std::cout.rdbuf(m_previous_stdout);
			std::cerr.rdbuf(m_previous_stderr);
		}

		StreamCapture(const StreamCapture&) = delete;
		StreamCapture& operator=(const StreamCapture&) = delete;

		std::string Stdout() const
		{
			return m_stdout.str();
		}

		std::string Stderr() const
		{
			return m_stderr.str();
		}
	};

	emscripten::val Report(const emscripten::val& errors)
	{
		emscripten::val report = emscripten::val::object();
		report.set("version", 1);
		report.set("source", std::string("marmot"));
		report.set("diagnostics", errors);
		report.set("errors", errors);
		report.set("warnings", emscripten::val::array());
		return report;
	}

	emscripten::val StartFailure(emscripten::val result, const std::string& message)
	{
		emscripten::val error = emscripten::val::object();
		error.set("severity", std::string("error"));
		error.set("stage", std::string("Module"));
		error.set("code", std::string("None"));
		error.set("message", message);
		emscripten::val errors = emscripten::val::array();
		errors.call<void>("push", error);
		result.set("report", Report(errors));
		result.set("error", message);
		return result;
	}

	emscripten::val RunMarmotBytecode(const emscripten::val& input)
	{
		const std::vector<uint8_t> bytes = emscripten::convertJSArrayToNumberVector<uint8_t>(input);
		std::istringstream artifact(std::string(bytes.begin(), bytes.end()), std::ios::in | std::ios::binary);
		std::expected<VmExecutable, std::string> program = VmBinaryArtifact::ReadExecutable(artifact);
		emscripten::val result = emscripten::val::object();
		result.set("version", 1);
		result.set("success", false);
		result.set("exitCode", 1);
		result.set("output", std::string());
		result.set("error", std::string());
		result.set("report", Report(emscripten::val::array()));
		if (!program.has_value())
		{
			return StartFailure(result, program.error());
		}
		const std::expected<void, std::string> libraries = MidoriProgramLoader::LoadNativeLibraries(program.value(), {});
		if (!libraries.has_value())
		{
			return StartFailure(result, libraries.error());
		}

		const StreamCapture capture;
		const std::expected<int, RuntimeError> executed = MidoriProgramLoader::Run(std::move(program).value());
		result.set("output", capture.Stdout());
		result.set("error", capture.Stderr());
		if (!executed.has_value())
		{
			result.set("exitCode", executed.error().ExitCode());
			result.set("error", capture.Stderr() + std::string(executed.error().Rendered()));
			emscripten::val errors = emscripten::val::array();
			errors.call<void>("push", emscripten::val::global("JSON").call<emscripten::val>("parse", SerializeMachineReadableRuntimeError(executed.error())));
			result.set("report", Report(errors));
			return result;
		}
		result.set("exitCode", executed.value());
		result.set("success", executed.value() == 0);
		return result;
	}
}

EMSCRIPTEN_BINDINGS(marmotvm_module)
{
	emscripten::function("runMarmotBytecode", &RunMarmotBytecode);
}
