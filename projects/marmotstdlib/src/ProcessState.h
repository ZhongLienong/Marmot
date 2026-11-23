#pragma once

#include <mutex>
#include <shared_mutex>

// All workers share one environment and working directory. Setters take an
// exclusive lock. Readers hold a shared lock for the whole operation, including
// copying getenv results, resolving relative paths, and launching a child.
// Consecutive calls can observe different process state.
namespace MarmotProcessState
{
	inline std::shared_mutex s_mutex;
}
