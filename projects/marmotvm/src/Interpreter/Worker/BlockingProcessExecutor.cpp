#include "BlockingProcessExecutor.h"

#include "BlockingProcessCall.h"

#include <algorithm>
#include <utility>

BlockingProcessExecutor::BlockingProcessExecutor(std::function<void(MarmotInteger)> complete)
	: m_complete(std::move(complete))
{
}

BlockingProcessExecutor::~BlockingProcessExecutor()
{
	Stop();
}

void BlockingProcessExecutor::Stop()
{
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_jobs.clear();
	}
	for (std::jthread& thread : m_threads)
	{
		thread.request_stop();
	}
}

void BlockingProcessExecutor::Submit(MarmotInteger id, std::shared_ptr<BlockingProcessCall> call)
{
	constexpr size_t max_helpers = 4u;
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_jobs.push_back({ id, std::move(call) });
		++m_pending;
		if (m_threads.size() < (std::min)(m_pending, max_helpers))
		{
			m_threads.emplace_back([this](std::stop_token stop) { Run(stop); });
		}
	}
	m_job_available.notify_one();
}

bool BlockingProcessExecutor::CancelQueued(MarmotInteger id)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	const std::deque<Job>::iterator job = std::ranges::find(m_jobs, id, &Job::m_id);
	if (job == m_jobs.end())
	{
		return false;
	}
	m_jobs.erase(job);
	--m_pending;
	return true;
}

void BlockingProcessExecutor::Run(std::stop_token stop)
{
	while (true)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_job_available.wait(lock, stop, [this]() { return !m_jobs.empty(); });
		if (stop.stop_requested())
		{
			return;
		}
		Job job = std::move(m_jobs.front());
		m_jobs.pop_front();
		lock.unlock();

		job.m_call->Run();

		lock.lock();
		--m_pending;
		lock.unlock();
		m_complete(job.m_id);
	}
}
