#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

class CompilationTimings
{
public:
	enum class Phase : size_t
	{
		Total,
		Lexing,
		Discovery,
		Schedule,
		Modules,
		Linking,
		Imports,
		SourceLines,
		Parsing,
		TypeChecking,
		StaticAnalysis,
		Lowering,
		Interface,
		Optimization,
		Backend,
		Finalization,
		Count
	};

	class Timer
	{
	private:
		CompilationTimings* m_timings;
		Phase m_phase;
		std::chrono::steady_clock::time_point m_started;

	public:
		Timer(CompilationTimings& timings, Phase phase);
		~Timer();
		Timer(const Timer&) = delete;
		Timer& operator=(const Timer&) = delete;
	};

	class ModuleTimer
	{
	private:
		CompilationTimings* m_timings;
		std::chrono::steady_clock::time_point m_started;

	public:
		explicit ModuleTimer(CompilationTimings& timings);
		~ModuleTimer();
		ModuleTimer(const ModuleTimer&) = delete;
		ModuleTimer& operator=(const ModuleTimer&) = delete;
	};

	explicit CompilationTimings(bool enabled);
	void SetWorkerCount(size_t workers);
	void Print() const;

private:
	bool m_enabled;
	std::array<std::atomic<int64_t>, static_cast<size_t>(Phase::Count)> m_durations{};
	std::atomic<int64_t> m_worker_time{ 0 };
	std::atomic<size_t> m_active_workers{ 0u };
	std::atomic<size_t> m_peak_workers{ 0u };
	std::atomic<size_t> m_modules_started{ 0u };
	size_t m_worker_count = 0u;

	void Record(Phase phase, std::chrono::steady_clock::duration duration);
	double Milliseconds(Phase phase) const;
};
