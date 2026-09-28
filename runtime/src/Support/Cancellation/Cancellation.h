#pragma once

#include <chrono>
#include <stop_token>

// Cancellation state for the thread currently running a worker. The builtin FFI layer
// has no access to the VM, so blocking builtins consult this to stay interruptible.
// Blocking calls that do not consult it (stdin reads, third-party dynamic FFI) still
// run to completion before the worker observes cancellation.
class ThreadCancellation
{
public:
	static void SetCurrentThreadStopToken(std::stop_token stop_token) noexcept;

	static void SleepInterruptible(std::chrono::milliseconds duration) noexcept;
};
