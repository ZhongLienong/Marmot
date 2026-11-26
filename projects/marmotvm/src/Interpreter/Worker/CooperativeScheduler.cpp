#include "CooperativeScheduler.h"

#include <utility>

CooperativeScheduler::CooperativeScheduler(size_t threads)
	: m_runnable(threads)
	, m_processes([this](MarmotInteger id) { ProcessCompleted(id); })
{
}

int CooperativeScheduler::CreateChannel(MarmotInteger capacity)
{
	return m_channels.CreateChannel(capacity);
}

ChannelOpStatus CooperativeScheduler::Send(MarmotInteger id, int channel, TransferPayload&& message, const std::stop_token& stop)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	const ChannelOpStatus status = m_channels.Send(channel, std::move(message), stop, ChannelWait::NonBlocking);
	if (status == ChannelOpStatus::WouldBlock)
	{
		ParkLocked(id, WorkerWait{ WorkerWait::Kind::ChannelSend, channel }, stop);
	}
	else if (status == ChannelOpStatus::Ok)
	{
		WakeOneLocked({ WorkerWait::Kind::ChannelReceive, channel });
	}
	return status;
}

ChannelReceiveResult CooperativeScheduler::Receive(MarmotInteger id, int channel, const std::stop_token& stop)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	ChannelReceiveResult result = m_channels.Receive(channel, stop, ChannelWait::NonBlocking);
	if (result.m_status == ChannelOpStatus::WouldBlock)
	{
		ParkLocked(id, WorkerWait{ WorkerWait::Kind::ChannelReceive, channel }, stop);
	}
	else if (result.m_status == ChannelOpStatus::Ok)
	{
		WakeOneLocked({ WorkerWait::Kind::ChannelSend, channel });
	}
	return result;
}

void CooperativeScheduler::Close(int channel)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	m_channels.Close(channel);
	WakeAllLocked({ WorkerWait::Kind::ChannelSend, channel });
	WakeAllLocked({ WorkerWait::Kind::ChannelReceive, channel });
}

void CooperativeScheduler::Enqueue(MarmotInteger id, size_t lane)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	m_lanes.emplace(id, lane);
	EnqueueLocked(id);
}

void CooperativeScheduler::FinishSlice(MarmotInteger id, const std::optional<WorkerSuspension>& suspension)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	--m_running_count;
	if (!suspension.has_value())
	{
		m_lanes.erase(id);
		WakeOneLocked({ WorkerWait::Kind::Join, id });
	}
	else if (std::holds_alternative<WorkerYield>(*suspension))
	{
		EnqueueLocked(id);
	}
	m_changed.notify_all();
}

void CooperativeScheduler::Sleep(MarmotInteger id, Clock::time_point deadline, const std::stop_token& stop)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	ParkLocked(id, deadline, stop);
}

void CooperativeScheduler::ParkLocked(MarmotInteger id, WorkerSuspension suspension, const std::stop_token& stop)
{
	if (stop.stop_requested())
	{
		EnqueueLocked(id);
		return;
	}
	m_waiting_workers.emplace(id, m_waiting.emplace(suspension, id));
}

void CooperativeScheduler::WakeOne(WorkerWait event)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	WakeOneLocked(event);
}

void CooperativeScheduler::WakeOneLocked(WorkerWait event)
{
	const WaitQueue::iterator waiter = m_waiting.lower_bound(event);
	if (waiter != m_waiting.end() && waiter->first == WorkerSuspension(event))
	{
		WakeLocked(waiter);
	}
}

void CooperativeScheduler::WakeAllLocked(WorkerWait event)
{
	auto [waiter, end] = m_waiting.equal_range(event);
	while (waiter != end)
	{
		WakeLocked(waiter++);
	}
}

void CooperativeScheduler::WakeWorker(MarmotInteger id)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	if (const WaitingWorkers::iterator waiter = m_waiting_workers.find(id); waiter != m_waiting_workers.end())
	{
		const WorkerWait* event = std::get_if<WorkerWait>(&waiter->second->first);
		if (event != nullptr && event->m_kind == WorkerWait::Kind::NativeProcess)
		{
			if (!m_processes.CancelQueued(id))
			{
				// A running native call reaches its cancellation point on completion.
				return;
			}
			--m_pending_processes;
		}
		WakeLocked(waiter->second);
	}
}

WorkerWait CooperativeScheduler::StartProcess(MarmotInteger id, std::shared_ptr<BlockingProcessCall> call, const std::stop_token& stop)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	const WorkerWait event{ WorkerWait::Kind::NativeProcess, id };
	if (stop.stop_requested())
	{
		EnqueueLocked(id);
	}
	else if (!m_stopped)
	{
		ParkLocked(id, event, {});
		++m_pending_processes;
		m_processes.Submit(id, std::move(call));
	}
	return event;
}

void CooperativeScheduler::ProcessCompleted(MarmotInteger id)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	--m_pending_processes;
	WakeOneLocked({ WorkerWait::Kind::NativeProcess, id });
}

std::optional<CooperativeScheduler::Clock::time_point> CooperativeScheduler::WakeExpiredTimersLocked()
{
	while (!m_waiting.empty())
	{
		const WaitQueue::iterator first = m_waiting.begin();
		const Clock::time_point* deadline = std::get_if<Clock::time_point>(&first->first);
		if (deadline == nullptr)
		{
			break;
		}
		if (*deadline > Clock::now())
		{
			return *deadline;
		}
		WakeLocked(first);
	}
	return std::nullopt;
}

std::optional<MarmotInteger> CooperativeScheduler::Next(size_t lane)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	while (!m_stopped)
	{
		const std::optional<Clock::time_point> timer = WakeExpiredTimersLocked();
		std::deque<MarmotInteger>& runnable = m_runnable[lane];
		if (!runnable.empty())
		{
			const MarmotInteger id = runnable.front();
			runnable.pop_front();
			--m_ready_count;
			++m_running_count;
			return id;
		}
		if (timer.has_value())
		{
			m_changed.wait_until(lock, *timer);
		}
		else if (m_ready_count != 0u || m_running_count != 0u || m_pending_processes != 0u)
		{
			m_changed.wait(lock);
		}
		else
		{
			m_stopped = true;
			m_changed.notify_all();
		}
	}
	return std::nullopt;
}

void CooperativeScheduler::Stop()
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	m_stopped = true;
	m_processes.Stop();
	m_changed.notify_all();
}

void CooperativeScheduler::EnqueueLocked(MarmotInteger id)
{
	m_runnable[m_lanes.at(id)].push_back(id);
	++m_ready_count;
	m_changed.notify_all();
}

void CooperativeScheduler::WakeLocked(WaitQueue::iterator waiter)
{
	const MarmotInteger id = waiter->second;
	m_waiting_workers.erase(id);
	m_waiting.erase(waiter);
	EnqueueLocked(id);
}
