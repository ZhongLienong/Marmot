#pragma once

#include <mutex>
#include <shared_mutex>

// Environment and working directory are process-wide. Hold a shared lock
// through operations that use them and an exclusive lock to change them.
namespace MarmotProcessState
{
	inline std::shared_mutex s_mutex;
}
