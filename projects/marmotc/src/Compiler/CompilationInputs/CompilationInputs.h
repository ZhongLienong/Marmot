#pragma once

#include "Bytecode/Executable/Executable.h"

#include <filesystem>
#include <optional>
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
private:
	std::vector<std::filesystem::path> m_search_paths;
	std::unordered_map<std::string, NativeLibraryPolicy> m_native_library_policies;
	bool m_emit_midori_ir = false;
	bool m_emit_ast = false;
	std::optional<size_t> m_jobs;
	bool m_emit_timings = false;

public:
	// Directories searched, in order, for `<Name>` imports. Directories that do
	// not exist are dropped and the rest made canonical, once, here.
	CompilationInputs WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&;

	const std::vector<std::filesystem::path>& SearchPaths() const;

	// Recorded in the program for each library it names; a library without one
	// is not thread-safe and has no checksum.
	CompilationInputs WithNativeLibraryPolicies(std::unordered_map<std::string, NativeLibraryPolicy> policies) &&;

	const std::unordered_map<std::string, NativeLibraryPolicy>& NativeLibraryPolicies() const;

	// Keep each module's MidoriIR as text in the compiled program.
	CompilationInputs WithEmitMidoriIR(bool emit_midori_ir) &&;

	bool EmitsMidoriIR() const;

	// Keep each module's checked syntax tree as text in the compiled program.
	CompilationInputs WithEmitAst(bool emit_ast) &&;

	bool EmitsAst() const;

	CompilationInputs WithJobs(std::optional<size_t> jobs) &&;

	const std::optional<size_t>& Jobs() const;

	CompilationInputs WithTimings(bool emit_timings) &&;

	bool EmitsTimings() const;
};
