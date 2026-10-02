#include "CompilationTimings.h"

#include <print>
#include <string_view>

CompilationTimings::CompilationTimings(bool enabled)
	: m_enabled(enabled)
{
}

CompilationTimings::Timer::Timer(CompilationTimings& timings, Phase phase)
	: m_timings(timings.m_enabled ? &timings : nullptr),
	m_phase(phase),
	m_started(m_timings != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
{
}

CompilationTimings::Timer::~Timer()
{
	if (m_timings != nullptr)
	{
		m_timings->Record(m_phase, std::chrono::steady_clock::now() - m_started);
	}
}

CompilationTimings::ModuleTimer::ModuleTimer(CompilationTimings& timings)
	: m_timings(timings.m_enabled ? &timings : nullptr),
	m_started(m_timings != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
{
	if (m_timings != nullptr)
	{
		m_timings->m_modules_started.fetch_add(1u, std::memory_order_relaxed);
		const size_t active = m_timings->m_active_workers.fetch_add(1u, std::memory_order_relaxed) + 1u;
		size_t peak = m_timings->m_peak_workers.load(std::memory_order_relaxed);
		while (peak < active && !m_timings->m_peak_workers.compare_exchange_weak(peak, active, std::memory_order_relaxed))
		{
		}
	}
}

CompilationTimings::ModuleTimer::~ModuleTimer()
{
	if (m_timings != nullptr)
	{
		const int64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_started).count();
		m_timings->m_worker_time.fetch_add(elapsed, std::memory_order_relaxed);
		m_timings->m_active_workers.fetch_sub(1u, std::memory_order_relaxed);
	}
}

void CompilationTimings::SetWorkerCount(size_t workers)
{
	m_worker_count = workers;
}

void CompilationTimings::Record(Phase phase, std::chrono::steady_clock::duration duration)
{
	m_durations[static_cast<size_t>(phase)].fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(), std::memory_order_relaxed);
}

double CompilationTimings::Milliseconds(Phase phase) const
{
	return static_cast<double>(m_durations[static_cast<size_t>(phase)].load(std::memory_order_relaxed)) / 1'000'000.0;
}

void CompilationTimings::Print() const
{
	if (!m_enabled)
	{
		return;
	}

	static constexpr std::array<std::string_view, static_cast<size_t>(Phase::Count)> s_names =
	{
		"total", "lexing", "discovery", "schedule", "modules", "linking",
		"imports", "source lines", "parsing", "type checking", "static analysis",
		"lowering + optimization", "bytecode backend", "finalization"
	};
	std::print(stderr, "Compilation timings (wall time):\n");
	for (size_t index = 0u; index < static_cast<size_t>(Phase::Imports); index += 1u)
	{
		std::print(stderr, "  {}: {:.3f} ms\n", s_names[index], Milliseconds(static_cast<Phase>(index)));
	}

	std::print(stderr, "Module stage totals (summed across workers):\n");
	for (size_t index = static_cast<size_t>(Phase::Imports); index < s_names.size(); index += 1u)
	{
		std::print(stderr, "  {}: {:.3f} ms\n", s_names[index], Milliseconds(static_cast<Phase>(index)));
	}

	std::print(stderr, "Workers: {} configured, {} peak active, {} modules started\n", m_worker_count, m_peak_workers.load(std::memory_order_relaxed), m_modules_started.load(std::memory_order_relaxed));
	const double modules_ms = Milliseconds(Phase::Modules);
	if (modules_ms > 0.0 && m_worker_count > 0u)
	{
		const double worker_ms = static_cast<double>(m_worker_time.load(std::memory_order_relaxed)) / 1'000'000.0;
		const double average = worker_ms / modules_ms;
		std::print(stderr, "Worker occupancy: {:.3f} average active, {:.1f}% of configured capacity\n", average, 100.0 * average / static_cast<double>(m_worker_count));
	}
}
