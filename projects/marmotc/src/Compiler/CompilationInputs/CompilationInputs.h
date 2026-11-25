#pragma once

#include "Bytecode/Executable/Executable.h"
#include "Support/Optional.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Everything the compiler learns from outside the source it is given: where
// `<Name>` imports are found, and how the native libraries the source names
// (`foreign ... from "library"`) may be used. The compiler reads no environment
// variables and no manifests; its caller (a build plan, or MARMOT_PATH for a
// file compiled on its own) resolves these.
class CompilationInputs
{
public:
	using SourceOverlays = std::unordered_map<std::string, std::string>;

private:
	std::vector<std::filesystem::path> m_search_paths;
	std::unordered_map<std::string, NativeLibraryPolicy> m_native_library_policies;
	std::shared_ptr<const SourceOverlays> m_source_overlays;
	bool m_emit_marmot_ir = false;
	bool m_emit_ast = false;
	MarmotCompiler::Optional<size_t> m_jobs;
	bool m_emit_timings = false;

public:
	// Directories searched, in order, for `<Name>` imports. Directories that do
	// not exist are dropped and the rest made canonical, once, here.
	CompilationInputs WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&;

	const std::vector<std::filesystem::path>& SearchPaths() const;

	// Immutable editor buffers shared by the import readers.
	CompilationInputs WithSourceOverlays(SourceOverlays overlays) &&;
	const std::string* SourceOverlay(const std::filesystem::path& file_path) const;

	// Recorded in the program for each library it names; a library without one
	// is not thread-safe and has no checksum.
	CompilationInputs WithNativeLibraryPolicies(std::unordered_map<std::string, NativeLibraryPolicy> policies) &&;

	const std::unordered_map<std::string, NativeLibraryPolicy>& NativeLibraryPolicies() const;

	// Keep each module's MarmotIR as text in the compiled program.
	CompilationInputs WithEmitMarmotIR(bool emit_marmot_ir) &&;

	bool EmitsMarmotIR() const;

	// Keep each module's checked syntax tree as text in the compiled program.
	CompilationInputs WithEmitAst(bool emit_ast) &&;

	bool EmitsAst() const;

	CompilationInputs WithJobs(MarmotCompiler::Optional<size_t> jobs) &&;

	const MarmotCompiler::Optional<size_t>& Jobs() const;

	CompilationInputs WithTimings(bool emit_timings) &&;

	bool EmitsTimings() const;
};
