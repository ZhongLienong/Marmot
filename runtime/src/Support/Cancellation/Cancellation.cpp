#include "Cancellation.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
	thread_local std::stop_token t_stop_token;
}

void ThreadCancellation::SetCurrentThreadStopToken(std::stop_token stop_token) noexcept
{
	t_stop_token = std::move(stop_token);
}

void ThreadCancellation::SleepInterruptible(std::chrono::milliseconds duration) noexcept
{
	if (!t_stop_token.stop_possible())
	{
		std::this_thread::sleep_for(duration);
		return;
	}

	std::mutex mutex;
	std::condition_variable_any wake_up;
	std::unique_lock<std::mutex> lock(mutex);
	static_cast<void>(wake_up.wait_for(lock, t_stop_token, duration, []()
	{
		return false;
	}));
}
