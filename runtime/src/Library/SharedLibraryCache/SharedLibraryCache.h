#pragma once

#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"

#include <expected>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct SharedLibraryEntry
{
	void* m_handle = nullptr;
	std::string m_package_name;
	std::filesystem::path m_path;
	bool m_thread_safe = false;
	std::unordered_map<std::string, FFIFunction> m_functions;
};

class SharedLibraryCache
{
public:
	static SharedLibraryCache& GetInstance();

	std::expected<void, std::string> LoadLibraryWithFunctions(
		const std::filesystem::path& library_path,
		const std::string& package_name,
		const std::unordered_map<std::string, std::string>& function_mappings,
		bool thread_safe,
		std::optional<std::string_view> expected_checksum = std::nullopt);

	bool IsLibraryLoaded(const std::string& package_name) const;

	std::unordered_map<std::string, FFIFunction> SnapshotAllFunctions() const;

	std::vector<std::string> GetNonThreadSafePackages() const;

	void Clear();

	~SharedLibraryCache();

private:
	SharedLibraryCache() = default;
	SharedLibraryCache(const SharedLibraryCache&) = delete;
	SharedLibraryCache& operator=(const SharedLibraryCache&) = delete;

	mutable std::mutex m_mutex;
	std::unordered_map<std::string, SharedLibraryEntry> m_libraries;

	static std::expected<void*, std::string> LoadPlatformLibrary(const std::filesystem::path& path);
	static void UnloadPlatformLibrary(void* handle);
	static void* GetPlatformFunction(void* library_handle, const std::string& function_name);
	static std::expected<void, std::string> VerifyLibraryChecksum(const std::filesystem::path& library_path, std::optional<std::string_view> expected_checksum);
};
