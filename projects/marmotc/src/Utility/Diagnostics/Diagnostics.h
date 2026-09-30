#pragma once

namespace CompilerDiagnostics
{
	class ScopedStatistics
	{
	private:
		bool m_previous;

	public:
		explicit ScopedStatistics(bool enabled) noexcept;
		~ScopedStatistics() noexcept;
		ScopedStatistics(const ScopedStatistics&) = delete;
		ScopedStatistics& operator=(const ScopedStatistics&) = delete;
	};

	[[nodiscard]] bool StatisticsEnabled() noexcept;
}
