#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "Bytecode/Artifact/BinaryArtifact.h"
#include "Compiler/Compiler.h"

namespace
{
	emscripten::val CompileMarmotCode(const std::string& source)
	{
		const CompilationInputs inputs = CompilationInputs().WithSearchPaths({std::filesystem::path("/MarmotPrelude")});
		MidoriResult::CompilationResult compiled = Compiler(std::string(source), std::string("/playground.mmt"), inputs).CompileWithReport();
		emscripten::val result = emscripten::val::object();
		result.set("version", 1);
		result.set("success", compiled.has_value());
		result.set("exitCode", compiled.has_value() ? 0 : 1);
		result.set("bytes", emscripten::val::global("Uint8Array").new_(0));
		const MidoriResult::CompilerReport& report = compiled.has_value() ? compiled->Report() : compiled.error();
		result.set("report", emscripten::val::global("JSON").call<emscripten::val>("parse", report.MachineReadableJson()));
		result.set("output", report.RenderedWarnings());
		result.set("error", report.RenderedErrors());
		if (!compiled.has_value())
		{
			return result;
		}

		std::ostringstream artifact(std::ios::out | std::ios::binary);
		const std::expected<void, std::string> written = MidoriBinaryArtifact::WriteExecutable(compiled->m_executable, artifact, true);
		if (!written.has_value())
		{
			result.set("success", false);
			result.set("exitCode", 1);
			result.set("error", written.error());
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
