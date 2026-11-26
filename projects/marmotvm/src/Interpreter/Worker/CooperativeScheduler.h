#pragma once

#include "Interpreter/Channel/Channel.h"
#include "Interpreter/Worker/BlockingProcessExecutor.h"
#include "Interpreter/Worker/Worker.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

class CooperativeScheduler
{
public:
	using Clock = std::chrono::steady_clock;

	explicit CooperativeScheduler(size_t threads);

	size_t ThreadCount() const noexcept { return m_runnable.size(); }
	int CreateChannel(MarmotInteger capacity);
	ChannelOpStatus Send(MarmotInteger id, int channel, TransferPayload&& message, const std::stop_token& stop);
	ChannelReceiveResult Receive(MarmotInteger id, int channel, const std::stop_token& stop);
	void Close(int channel);

	void Enqueue(MarmotInteger id, size_t lane);
	void FinishSlice(MarmotInteger id, const std::optional<WorkerSuspension>& suspension);
	void Sleep(MarmotInteger id, Clock::time_point deadline, const std::stop_token& stop);
	void WakeOne(WorkerWait event);
	void WakeWorker(MarmotInteger id);
	WorkerWait StartProcess(MarmotInteger id, std::shared_ptr<BlockingProcessCall> call, const std::stop_token& stop);

	std::optional<MarmotInteger> Next(size_t lane);
	void Stop();

private:
	friend class WorkerRegistry;

	// The scheduler mutex covers the readiness check and parking for a join or
	// channel operation. A woken VM goes back to the lane that last ran it.
	using WaitQueue = std::multimap<WorkerSuspension, MarmotInteger>;
	using WaitingWorkers = std::unordered_map<MarmotInteger, WaitQueue::iterator>;

	void ParkLocked(MarmotInteger id, WorkerSuspension suspension, const std::stop_token& stop);
	void EnqueueLocked(MarmotInteger id);
	void WakeLocked(WaitQueue::iterator waiter);
	void WakeOneLocked(WorkerWait event);
	void WakeAllLocked(WorkerWait event);
	std::optional<Clock::time_point> WakeExpiredTimersLocked();
	void ProcessCompleted(MarmotInteger id);

	ChannelRegistry m_channels;
	std::mutex m_mutex;
	std::condition_variable m_changed;
	std::vector<std::deque<MarmotInteger>> m_runnable;
	std::unordered_map<MarmotInteger, size_t> m_lanes;
	WaitQueue m_waiting;
	WaitingWorkers m_waiting_workers;
	size_t m_ready_count = 0u;
	size_t m_running_count = 0u;
	size_t m_pending_processes = 0u;
	bool m_stopped = false;
	BlockingProcessExecutor m_processes;
};
