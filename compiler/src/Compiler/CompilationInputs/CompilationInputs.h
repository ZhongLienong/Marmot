#pragma once

#include <filesystem>
#include <vector>

// Everything the compiler learns from outside the source it is given: where
// `<Name>` imports are found. The compiler reads no environment variables and
// no manifests; its caller (a build plan, or MARMOT_PATH for a file compiled on
// its own) resolves these. Native libraries are named in the source itself
// (`foreign ... from "library"`), so they are not inputs.
class CompilationInputs
{
private:
	std::vector<std::filesystem::path> m_search_paths;

public:
	// Directories searched, in order, for `<Name>` imports. Directories that do
	// not exist are dropped and the rest made canonical, once, here.
	CompilationInputs WithSearchPaths(const std::vector<std::filesystem::path>& search_paths) &&;

	const std::vector<std::filesystem::path>& SearchPaths() const;
};
