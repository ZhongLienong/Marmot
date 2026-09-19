#pragma once

#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"
#include "Common/Executable/Executable.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class DynamicFFIRegistry
{
public:
	DynamicFFIRegistry();

	DynamicFFIRegistry(const DynamicFFIRegistry&) = delete;
	DynamicFFIRegistry& operator=(const DynamicFFIRegistry&) = delete;
	DynamicFFIRegistry(DynamicFFIRegistry&&) noexcept = default;
	DynamicFFIRegistry& operator=(DynamicFFIRegistry&&) noexcept = default;

	void SnapshotFromCache();

	std::optional<FFIFunction> FindFunction(std::string_view function_name) const;

	// Only the libraries the program imports count: another program in the same
	// process may have loaded others.
	static std::expected<void, std::string> ValidateWorkerSafety(const std::vector<NativeLibraryImport>& libraries);

private:
	std::unordered_map<std::string, FFIFunction> m_functions;
};
