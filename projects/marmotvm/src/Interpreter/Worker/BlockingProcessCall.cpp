#include "BlockingProcessCall.h"

#include <array>
#include <utility>

BlockingProcessCall::BlockingProcessCall(const VmNativeFunction& function, const StdlibWorkerErrorHooks& hooks,
	const MarmotFFIArray& arguments, const char* directory)
	: m_function(function)
	, m_hooks(hooks)
	, m_directory(directory)
	, m_worker_errors(nullptr, hooks.m_free)
{
	const char* const* strings = static_cast<const char* const*>(arguments.data);
	m_arguments.reserve(static_cast<size_t>(arguments.length));
	for (int32_t index = 0; index < arguments.length; ++index)
	{
		m_arguments.emplace_back(strings[index]);
	}
	m_worker_errors.reset(m_hooks.m_exchange(nullptr));
}

BlockingProcessCall::~BlockingProcessCall()
{
	if (m_result != nullptr)
	{
		m_function.m_free_result(MARMOT_FFI_RETURN_ARRAY_STRINGS, m_result);
	}
}

void BlockingProcessCall::Run() noexcept
{
	std::vector<const char*> strings;
	strings.reserve(m_arguments.size());
	for (const std::string& argument : m_arguments)
	{
		strings.push_back(argument.c_str());
	}
	MarmotFFIArray arguments{ strings.data(), static_cast<int32_t>(strings.size()) };
	std::array<void*, 2u> args{ &arguments, m_directory.data() };

	void* const previous = m_hooks.m_exchange(m_worker_errors.release());
	m_function.m_call(args.data(), &m_result);
	m_worker_errors.reset(m_hooks.m_exchange(previous));
}

BlockingProcessCall::WorkerErrors BlockingProcessCall::TakeWorkerErrors() noexcept
{
	return std::move(m_worker_errors);
}

MarmotFFIArray* BlockingProcessCall::ReleaseResult() noexcept
{
	return std::exchange(m_result, nullptr);
}
