#pragma once

#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <memory>
#include <string>
#include <vector>

class BlockingProcessCall
{
public:
	using WorkerErrors = std::unique_ptr<void, StdlibWorkerErrorHooks::Free>;

	BlockingProcessCall(const VmNativeFunction& function, const StdlibWorkerErrorHooks& hooks,
		const VmNativeAbi::Array& arguments, const char* directory);
	~BlockingProcessCall();

	void Run() noexcept;
	WorkerErrors TakeWorkerErrors() noexcept;
	void* ReleaseResult() noexcept;
	VmNativeAbi::FreeResult GetFreeResult() const noexcept { return m_function.m_free_result; }

private:
	VmNativeFunction m_function;
	StdlibWorkerErrorHooks m_hooks;
	std::vector<std::string> m_arguments;
	std::string m_directory;
	WorkerErrors m_worker_errors;
	void* m_result = nullptr;
};
