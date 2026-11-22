#include "DynamicFFIRegistry.h"

#include <format>
#include <ranges>
#include <string_view>

DynamicFFIRegistry::DynamicFFIRegistry()
	: m_functions(SharedLibraryCache::GetInstance().SnapshotAllFunctions())
{
}

const VmNativeFunction* DynamicFFIRegistry::FindFunction(std::string_view function_name) const
{
	const auto it = m_functions.find(std::string(function_name));
	return it == m_functions.end() ? nullptr : &it->second;
}

std::expected<void, std::string> DynamicFFIRegistry::ValidateWorkerSafety(const std::vector<VmNativeLibraryImport>& libraries)
{
	std::vector<std::string> library_names;
	library_names.reserve(libraries.size());
	for (const VmNativeLibraryImport& library : libraries)
	{
		if (!library.m_policy.m_thread_safe)
		{
			library_names.push_back(library.m_name);
		}
	}

	if (library_names.empty())
	{
		return {};
	}

	const std::string package_list = library_names
		| std::views::transform([](const std::string& library_name) { return std::format("'{}'", library_name); })
		| std::views::join_with(std::string_view(", "))
		| std::ranges::to<std::string>();

	return std::unexpected(std::format(
		"Cannot spawn worker: the following native libraries are not declared thread_safe: {}. "
		"Set thread_safe = true in the [ffi] section of the package.marmot that ships each one if its native code is safe for concurrent use.",
		package_list));
}
