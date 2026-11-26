#pragma once

#include "Value/Value.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

class BlockingProcessCall;

class BlockingProcessExecutor
{
public:
	explicit BlockingProcessExecutor(std::function<void(MarmotInteger)> complete);
	~BlockingProcessExecutor();

	void Submit(MarmotInteger id, std::shared_ptr<BlockingProcessCall> call);
	bool CancelQueued(MarmotInteger id);
	void Stop();

private:
	struct Job
	{
		MarmotInteger m_id;
		std::shared_ptr<BlockingProcessCall> m_call;
	};

	void Run(std::stop_token stop);

	std::mutex m_mutex;
	std::condition_variable_any m_job_available;
	std::deque<Job> m_jobs;
	std::function<void(MarmotInteger)> m_complete;
	size_t m_pending = 0u;
	// Join the helpers before destroying their queues or condition variables.
	std::vector<std::jthread> m_threads;
};
