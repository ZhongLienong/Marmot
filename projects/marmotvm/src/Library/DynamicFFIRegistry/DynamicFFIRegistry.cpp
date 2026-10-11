#include "DynamicFFIRegistry.h"

#include <format>
#include <ranges>
#include <string_view>

DynamicFFIRegistry::DynamicFFIRegistry()
{
	SnapshotFromCache();
}

void DynamicFFIRegistry::SnapshotFromCache()
{
	m_functions = SharedLibraryCache::GetInstance().SnapshotAllFunctions();
}

std::optional<VmFFIFunction> DynamicFFIRegistry::FindFunction(std::string_view function_name) const
{
	const std::unordered_map<std::string, VmFFIFunction>::const_iterator it = m_functions.find(std::string(function_name));
	if (it != m_functions.end())
	{
		return it->second;
	}

	return std::nullopt;
}

std::expected<void, std::string> DynamicFFIRegistry::ValidateWorkerSafety(const std::vector<VmNativeLibraryImport>& libraries)
{
	std::vector<std::string> library_names;
	library_names.reserve(libraries.size());
	for (const VmNativeLibraryImport& library : libraries)
	{
		library_names.push_back(library.m_name);
	}

	const std::vector<std::string> non_thread_safe = SharedLibraryCache::GetInstance().GetNonThreadSafeLibraries(library_names);
	if (non_thread_safe.empty())
	{
		return {};
	}

	const std::string package_list = non_thread_safe
		| std::views::transform([](const std::string& library_name) { return std::format("'{}'", library_name); })
		| std::views::join_with(std::string_view(", "))
		| std::ranges::to<std::string>();

	return std::unexpected(std::format(
		"Cannot spawn worker: the following native libraries are not declared thread_safe: {}. "
		"Set thread_safe = true in the [ffi] section of the package.marmot that ships each one if its native code is safe for concurrent use.",
		package_list));
}
