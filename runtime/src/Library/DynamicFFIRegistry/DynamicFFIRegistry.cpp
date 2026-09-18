#include "DynamicFFIRegistry.h"

#include <format>

DynamicFFIRegistry::DynamicFFIRegistry()
{
	SnapshotFromCache();
}

void DynamicFFIRegistry::SnapshotFromCache()
{
	m_functions = SharedLibraryCache::GetInstance().SnapshotAllFunctions();
}

std::optional<FFIFunction> DynamicFFIRegistry::FindFunction(std::string_view function_name) const
{
	const std::unordered_map<std::string, FFIFunction>::const_iterator it = m_functions.find(std::string(function_name));
	if (it != m_functions.end())
	{
		return it->second;
	}

	return std::nullopt;
}

std::expected<void, std::string> DynamicFFIRegistry::ValidateWorkerSafety() const
{
	const std::vector<std::string> non_thread_safe = SharedLibraryCache::GetInstance().GetNonThreadSafePackages();
	if (non_thread_safe.empty())
	{
		return {};
	}

	std::string package_list;
	for (size_t i = 0uz; i < non_thread_safe.size(); i += 1uz)
	{
		if (i > 0uz)
		{
			package_list += ", ";
		}
		package_list += "'" + non_thread_safe[i] + "'";
	}

	return std::unexpected(std::format(
		"Cannot spawn worker: the following FFI packages are not declared thread_safe: {}. "
		"Set thread_safe = true in each package's package.marmot [ffi] section if the native code is safe for concurrent use.",
		package_list));
}
