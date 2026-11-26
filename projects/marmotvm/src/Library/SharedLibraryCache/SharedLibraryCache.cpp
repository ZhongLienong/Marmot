#include "SharedLibraryCache.h"
#include "Bytecode/Executable/Executable.h"
#include "Support/Checksum.h"

#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#undef LoadLibrary
#elif !defined(__EMSCRIPTEN__)
	#include <dlfcn.h>
#endif

#include <format>

SharedLibraryCache& SharedLibraryCache::GetInstance()
{
	static SharedLibraryCache instance;
	return instance;
}

namespace
{
	template <typename Resolver>
	std::expected<std::unordered_map<std::string, VmNativeFunction>, std::string> BindSymbols(
		const std::string& name, const std::vector<std::string>& symbols,
		MarmotFFIFreeResultFn free_result, const std::shared_ptr<void>& library, Resolver resolve)
	{
		if (free_result == nullptr)
		{
			return std::unexpected(std::format("FFI error: native library '{}' does not export 'MARMOT_FFI_FreeResult'.", name));
		}

		std::unordered_map<std::string, VmNativeFunction> functions;
		for (const std::string& symbol : symbols)
		{
			MarmotFFIFn call = resolve(symbol);
			if (call == nullptr)
			{
				return std::unexpected(std::format("FFI error: native library '{}' does not export '{}'.", name, symbol));
			}
			functions.emplace(name + VM_NATIVE_SYMBOL_SEPARATOR + symbol, VmNativeFunction{call, free_result, library});
		}
		return functions;
	}
}

std::expected<void, std::string> SharedLibraryCache::LoadLibraryWithFunctions(
	const std::filesystem::path& library_path,
	const std::string& name,
	const std::vector<std::string>& symbols,
	std::optional<std::string_view> expected_checksum)
{
	const auto checksum = VerifyLibraryChecksum(library_path, expected_checksum);
	if (!checksum.has_value())
	{
		return std::unexpected(checksum.error());
	}

	const auto loaded = LoadPlatformLibrary(library_path);
	if (!loaded.has_value())
	{
		return std::unexpected(loaded.error());
	}

	const std::shared_ptr<void> handle(loaded.value(), UnloadPlatformLibrary);
	const auto free_result = reinterpret_cast<MarmotFFIFreeResultFn>(GetPlatformFunction(handle.get(), "MARMOT_FFI_FreeResult"));
	auto bound = BindSymbols(name, symbols, free_result, handle, [&handle](const std::string& symbol)
	{
		return reinterpret_cast<MarmotFFIFn>(GetPlatformFunction(handle.get(), symbol));
	});
	if (!bound.has_value())
	{
		return std::unexpected(bound.error());
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	for (const auto& [key, function] : bound.value())
	{
		m_functions.insert_or_assign(key, function);
	}
	return {};
}

void SharedLibraryCache::RegisterHostLibrary(std::string name, MarmotFFIFreeResultFn free_result, HostSymbolResolver resolve)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_host_libraries.emplace(std::move(name), HostLibrary{free_result, resolve});
}

std::optional<std::expected<void, std::string>> SharedLibraryCache::LoadHostLibrary(
	const std::string& name, const std::vector<std::string>& symbols)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto found = m_host_libraries.find(name);
	if (found == m_host_libraries.end())
	{
		return std::nullopt;
	}
	const HostLibrary& host = found->second;
	auto bound = BindSymbols(name, symbols, host.m_free_result, {}, host.m_resolve);
	if (!bound.has_value())
	{
		return std::unexpected(bound.error());
	}
	m_functions.merge(bound.value());
	return std::expected<void, std::string>{};
}

std::unordered_map<std::string, VmNativeFunction> SharedLibraryCache::SnapshotAllFunctions() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_functions;
}

void SharedLibraryCache::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_functions.clear();
}

std::optional<StdlibWorkerErrorHooks> SharedLibraryCache::FindStdlibWorkerErrorHooks(const VmNativeFunction& function)
{
	void* const library = function.m_library.get();
	const StdlibWorkerErrorHooks::Exchange exchange = reinterpret_cast<StdlibWorkerErrorHooks::Exchange>(GetPlatformFunction(library, "MARMOT_STDLIB_ExchangeWorkerErrors"));
	const StdlibWorkerErrorHooks::Free free = reinterpret_cast<StdlibWorkerErrorHooks::Free>(GetPlatformFunction(library, "MARMOT_STDLIB_FreeWorkerErrors"));
	if (exchange == nullptr || free == nullptr)
	{
		return std::nullopt;
	}
	return StdlibWorkerErrorHooks{ exchange, free };
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
#elif defined(__EMSCRIPTEN__)
	return std::unexpected(std::format("FFI error: browser host has not registered native library file '{}'.", path.string()));
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
#elif !defined(__EMSCRIPTEN__)
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
#elif defined(__EMSCRIPTEN__)
	return nullptr;
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

	const std::expected<bool, std::string> verification = MarmotChecksum::VerifyFileChecksum(library_path, *expected_checksum);
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
