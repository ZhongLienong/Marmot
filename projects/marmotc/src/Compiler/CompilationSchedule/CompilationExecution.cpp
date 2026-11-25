#include "CompilationExecution.h"
#include "Support/Optional.h"

#include <condition_variable>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	class Executor
	{
	public:
		Executor(CompilationSchedule& schedule, const CompilationExecution::CompileModule& compile)
			: m_schedule(schedule),
			m_compile(compile)
		{
			Enqueue(m_schedule.InitialReady());
			m_results.reserve(m_schedule.m_all_modules.size());
		}

		CompilationExecution::Results Run(size_t worker_count)
		{
			{
#ifndef __EMSCRIPTEN__
				// Unwinding a partial startup requests stop and wakes each waiting worker.
				std::vector<std::jthread> workers;
				workers.reserve(worker_count);
				for (size_t index = 1u; index < worker_count; index += 1u)
				{
					workers.emplace_back([this](std::stop_token stop) { WorkerLoop(stop); });
				}
#else
				(void)worker_count;
#endif
				WorkerLoop(std::stop_token{});
			}
			return std::move(m_results);
		}

	private:
		void Enqueue(std::vector<ScheduledModule> modules)
		{
			for (ScheduledModule& module : modules)
			{
				m_ready.push(std::move(module));
			}
		}

		void PublishInterface(const std::string& file_path)
		{
			size_t ready_count;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				std::vector<ScheduledModule> ready = m_schedule.InterfaceReady(file_path);
				ready_count = ready.size();
				Enqueue(std::move(ready));
			}
			if (ready_count == 1u)
			{
				m_ready_cv.notify_one();
			}
			else if (ready_count > 1u)
			{
				m_ready_cv.notify_all();
			}
		}

		void WorkerLoop(std::stop_token stop)
		{
			const CompilationExecution::InterfaceReady interface_ready = [this](const std::string& file_path) { PublishInterface(file_path); };
			while (true)
			{
				MarmotCompiler::Optional<ScheduledModule> module;
				{
					std::unique_lock<std::mutex> lock(m_mutex);
					m_ready_cv.wait(lock, stop, [this]() { return !m_ready.empty() || m_active == 0u; });
					if (stop.stop_requested() || m_ready.empty())
					{
						return;
					}
					module.Emplace(m_ready.top());
					m_ready.pop();
					m_active += 1u;
				}

				MarmotResult::CompiledModuleReportResult result = m_compile(*module, interface_ready);
				bool finished;
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_results.emplace(module->m_file_path, std::move(result));
					m_active -= 1u;
					finished = m_ready.empty() && m_active == 0u;
				}
				if (finished)
				{
					m_ready_cv.notify_all();
				}
			}
		}

		CompilationSchedule& m_schedule;
		const CompilationExecution::CompileModule& m_compile;
		std::mutex m_mutex;
		std::condition_variable_any m_ready_cv;
		std::priority_queue<ScheduledModule> m_ready;
		size_t m_active = 0u;
		CompilationExecution::Results m_results;
	};
}

CompilationExecution::Results CompilationExecution::Run(CompilationSchedule& schedule, size_t worker_count, const CompileModule& compile)
{
	return Executor(schedule, compile).Run(worker_count);
}
