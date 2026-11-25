#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Compiler/Compiler.h"
#include "Support/Expected.h"

namespace
{
	emscripten::val CompileMarmotCode(const std::string& source)
	{
		const CompilationInputs inputs = CompilationInputs().WithSearchPaths({std::filesystem::path("/MarmotPrelude")});
		MarmotResult::CompiledProgramReportResult compiled = Compiler(std::string(source), std::string("/playground.mmt"), inputs).CompileWithReport();
		emscripten::val result = emscripten::val::object();
		result.set("version", 1);
		result.set("success", compiled.HasValue());
		result.set("exitCode", compiled.HasValue() ? 0 : 1);
		result.set("bytes", emscripten::val::global("Uint8Array").new_(0));
		const MarmotResult::CompilerReport& report = compiled.HasValue() ? compiled->Report() : compiled.Error();
		result.set("report", emscripten::val::global("JSON").call<emscripten::val>("parse", report.MachineReadableJson()));
		result.set("output", report.RenderedWarnings());
		result.set("error", report.m_errors.Rendered());
		if (!compiled.HasValue())
		{
			return result;
		}

		std::ostringstream artifact(std::ios::out | std::ios::binary);
		const MarmotCompiler::Expected<void, std::string> written = MarmotBinaryArtifact::WriteExecutable(compiled->m_executable, artifact, true);
		if (!written.HasValue())
		{
			result.set("success", false);
			result.set("exitCode", 1);
			result.set("error", written.Error());
			return result;
		}
		const std::string bytes = artifact.str();
		// The result owns a copy: it must survive this string and later compiler calls.
		result.set("bytes", emscripten::val::global("Uint8Array").new_(
			emscripten::val(emscripten::typed_memory_view(bytes.size(), reinterpret_cast<const unsigned char*>(bytes.data())))));
		return result;
	}
}

EMSCRIPTEN_BINDINGS(marmotc_module)
{
	emscripten::function("compileMarmotCode", &CompileMarmotCode);
}
