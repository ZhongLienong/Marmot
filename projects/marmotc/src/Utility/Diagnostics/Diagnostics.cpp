#include "Diagnostics.h"

#include <atomic>

namespace
{
	std::atomic<bool> s_statistics = false;
}

CompilerDiagnostics::ScopedStatistics::ScopedStatistics(bool enabled) noexcept
	: m_previous(s_statistics.exchange(enabled))
{
}

CompilerDiagnostics::ScopedStatistics::~ScopedStatistics() noexcept
{
	s_statistics.store(m_previous);
}

bool CompilerDiagnostics::StatisticsEnabled() noexcept
{
	return s_statistics.load();
}
