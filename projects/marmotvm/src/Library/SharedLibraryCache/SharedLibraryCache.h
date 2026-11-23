#pragma once

#include "marmot/MarmotFFI.h"

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct VmNativeFunction
{
	MarmotFFIFn m_call;
	MarmotFFIFreeResultFn m_free_result;
	std::shared_ptr<void> m_library;
};

class SharedLibraryCache
{
public:
	using HostSymbolResolver = MarmotFFIFn(*)(std::string_view);

	static SharedLibraryCache& GetInstance();

	std::expected<void, std::string> LoadLibraryWithFunctions(
		const std::filesystem::path& library_path,
		const std::string& package_name,
		const std::vector<std::string>& symbols,
		std::optional<std::string_view> expected_checksum = std::nullopt);

	void RegisterHostLibrary(std::string name, MarmotFFIFreeResultFn free_result, HostSymbolResolver resolve);
	std::optional<std::expected<void, std::string>> LoadHostLibrary(
		const std::string& name, const std::vector<std::string>& symbols);
	std::unordered_map<std::string, VmNativeFunction> SnapshotAllFunctions() const;

	void Clear();

private:
	SharedLibraryCache() = default;
	SharedLibraryCache(const SharedLibraryCache&) = delete;
	SharedLibraryCache& operator=(const SharedLibraryCache&) = delete;

	struct HostLibrary
	{
		MarmotFFIFreeResultFn m_free_result;
		HostSymbolResolver m_resolve;
	};

	mutable std::mutex m_mutex;
	std::unordered_map<std::string, HostLibrary> m_host_libraries;
	std::unordered_map<std::string, VmNativeFunction> m_functions;

	static std::expected<void*, std::string> LoadPlatformLibrary(const std::filesystem::path& path);
	static void UnloadPlatformLibrary(void* handle);
	static void* GetPlatformFunction(void* library_handle, const std::string& function_name);
	static std::expected<void, std::string> VerifyLibraryChecksum(const std::filesystem::path& library_path, std::optional<std::string_view> expected_checksum);
};
