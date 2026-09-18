#include "SharedLibraryCache.h"
#include "Common/Checksum/Checksum.h"

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#undef LoadLibrary
#else
	#include <dlfcn.h>
#endif

#include <format>

SharedLibraryCache& SharedLibraryCache::GetInstance()
{
	static SharedLibraryCache instance;
	return instance;
}

std::expected<void, std::string> SharedLibraryCache::LoadLibraryWithFunctions(
	const std::filesystem::path& library_path,
	const std::string& package_name,
	const std::unordered_map<std::string, std::string>& function_mappings,
	bool thread_safe,
	std::optional<std::string_view> expected_checksum)
{
	std::lock_guard<std::mutex> lock(m_mutex);

	if (m_libraries.contains(package_name))
	{
		return std::unexpected(std::format("FFI error: package '{}' already has a loaded native library.", package_name));
	}

	if (!std::filesystem::exists(library_path))
	{
		return std::unexpected(std::format("FFI error: library file not found: {}", library_path.string()));
	}

	const std::expected<void, std::string> checksum_result = VerifyLibraryChecksum(library_path, expected_checksum);
	if (!checksum_result.has_value())
	{
		return std::unexpected(checksum_result.error());
	}

	const std::expected<void*, std::string> load_result = LoadPlatformLibrary(library_path);
	if (!load_result.has_value())
	{
		return std::unexpected(load_result.error());
	}

	void* handle = load_result.value();
	std::unordered_map<std::string, FFIFunction> resolved_functions;

	for (const std::pair<const std::string, std::string>& func_pair : function_mappings)
	{
		const std::string& midori_name = func_pair.first;
		const std::string& native_name = func_pair.second;

		void* func_ptr = GetPlatformFunction(handle, native_name);
		if (func_ptr == nullptr)
		{
			UnloadPlatformLibrary(handle);
			return std::unexpected(std::format(
				"FFI error: package '{}' declares function '{}' but symbol '{}' was not found in {}",
				package_name,
				midori_name,
				native_name,
				library_path.string()));
		}

		resolved_functions.emplace(midori_name, reinterpret_cast<FFIFunction>(func_ptr));
	}

	SharedLibraryEntry entry;
	entry.m_handle = handle;
	entry.m_package_name = package_name;
	entry.m_path = library_path;
	entry.m_thread_safe = thread_safe;
	entry.m_functions = std::move(resolved_functions);
	m_libraries.emplace(package_name, std::move(entry));

	return {};
}

bool SharedLibraryCache::IsLibraryLoaded(const std::string& package_name) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_libraries.contains(package_name);
}

std::unordered_map<std::string, FFIFunction> SharedLibraryCache::SnapshotAllFunctions() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::unordered_map<std::string, FFIFunction> snapshot;
	for (const std::pair<const std::string, SharedLibraryEntry>& lib : m_libraries)
	{
		for (const std::pair<const std::string, FFIFunction>& func : lib.second.m_functions)
		{
			snapshot.emplace(func.first, func.second);
		}
	}
	return snapshot;
}

std::vector<std::string> SharedLibraryCache::GetNonThreadSafePackages() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<std::string> result;
	for (const std::pair<const std::string, SharedLibraryEntry>& lib : m_libraries)
	{
		if (!lib.second.m_thread_safe)
		{
			result.push_back(lib.second.m_package_name);
		}
	}
	return result;
}

void SharedLibraryCache::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const std::pair<const std::string, SharedLibraryEntry>& lib : m_libraries)
	{
		UnloadPlatformLibrary(lib.second.m_handle);
	}
	m_libraries.clear();
}

SharedLibraryCache::~SharedLibraryCache()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const std::pair<const std::string, SharedLibraryEntry>& lib : m_libraries)
	{
		UnloadPlatformLibrary(lib.second.m_handle);
	}
}

std::expected<void*, std::string> SharedLibraryCache::LoadPlatformLibrary(const std::filesystem::path& path)
{
#ifdef _WIN32
	void* handle = LoadLibraryW(path.c_str());
	if (!handle)
	{
		DWORD error = GetLastError();
		return std::unexpected(std::format("FFI error: failed to load library '{}' (Windows error {}).", path.string(), error));
	}
	return handle;
#else
	void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
	if (!handle)
	{
		return std::unexpected(std::format("FFI error: failed to load library '{}' ({}).", path.string(), dlerror()));
	}
	return handle;
#endif
}

void SharedLibraryCache::UnloadPlatformLibrary(void* handle)
{
	if (!handle)
	{
		return;
	}

#ifdef _WIN32
	FreeLibrary(static_cast<HMODULE>(handle));
#else
	dlclose(handle);
#endif
}

void* SharedLibraryCache::GetPlatformFunction(void* library_handle, const std::string& function_name)
{
	if (!library_handle)
	{
		return nullptr;
	}

#ifdef _WIN32
	return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library_handle), function_name.c_str()));
#else
	return dlsym(library_handle, function_name.c_str());
#endif
}

std::expected<void, std::string> SharedLibraryCache::VerifyLibraryChecksum(const std::filesystem::path& library_path, std::optional<std::string_view> expected_checksum)
{
	if (!expected_checksum.has_value() || expected_checksum->empty())
	{
		return {};
	}

	const std::expected<bool, std::string> verification = MidoriChecksum::VerifyFileChecksum(library_path, *expected_checksum);
	if (!verification.has_value())
	{
		return std::unexpected(std::format(
			"FFI error: failed to verify checksum for {}: {}",
			library_path.string(),
			verification.error()));
	}

	if (!verification.value())
	{
		return std::unexpected(std::format(
			"FFI error: checksum mismatch for {}. Refusing to load the library.",
			library_path.string()));
	}

	return {};
}
