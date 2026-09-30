#include "Channel.h"

#include <memory>

Channel::Channel(int capacity)
	: m_capacity(capacity > 0 ? capacity : 1)
{
}

ChannelOpStatus Channel::Send(SerializedValue message, std::stop_token stop_token)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	const bool ready = m_not_full.wait(lock, stop_token, [this]()
	{
		return m_closed || static_cast<int>(m_queue.size()) < m_capacity;
	});

	if (!ready)
	{
		return ChannelOpStatus::Cancelled;
	}

	if (m_closed)
	{
		return ChannelOpStatus::Closed;
	}

	m_queue.push_back(std::move(message));
	m_not_empty.notify_one();
	return ChannelOpStatus::Ok;
}

ChannelReceiveResult Channel::Receive(std::stop_token stop_token)
{
	std::unique_lock<std::mutex> lock(m_mutex);
	const bool ready = m_not_empty.wait(lock, stop_token, [this]()
	{
		return !m_queue.empty() || m_closed;
	});

	if (!ready)
	{
		return ChannelReceiveResult{ ChannelOpStatus::Cancelled, std::nullopt };
	}

	if (m_queue.empty())
	{
		return ChannelReceiveResult{ ChannelOpStatus::Closed, std::nullopt };
	}

	SerializedValue message = std::move(m_queue.front());
	m_queue.pop_front();
	m_not_full.notify_one();
	return ChannelReceiveResult{ ChannelOpStatus::Ok, std::move(message) };
}

std::optional<SerializedValue> Channel::TryReceive()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_queue.empty())
	{
		return std::nullopt;
	}

	SerializedValue message = std::move(m_queue.front());
	m_queue.pop_front();
	m_not_full.notify_one();
	return message;
}

void Channel::Close()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_closed = true;
	m_close_requested.store(true, std::memory_order_release);
	m_not_empty.notify_all();
	m_not_full.notify_all();
}

bool Channel::IsClosed() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_closed;
}

bool Channel::IsDrained() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_closed && m_queue.empty();
}

bool Channel::IsCloseRequested() const noexcept
{
	return m_close_requested.load(std::memory_order_acquire);
}

ChannelRegistry& ChannelRegistry::GetInstance()
{
	static ChannelRegistry instance;
	return instance;
}

int ChannelRegistry::CreateChannel(int capacity)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const int id = m_next_id;
	m_next_id += 1;
	m_channels.emplace(id, std::make_shared<Channel>(capacity));
	return id;
}

ChannelOpStatus ChannelRegistry::Send(int channel_id, SerializedValue message, std::stop_token stop_token)
{
	std::shared_ptr<Channel> channel = FindChannel(channel_id);
	if (channel == nullptr)
	{
		return ChannelOpStatus::Closed;
	}
	return channel->Send(std::move(message), std::move(stop_token));
}

ChannelReceiveResult ChannelRegistry::Receive(int channel_id, std::stop_token stop_token)
{
	std::shared_ptr<Channel> channel = FindChannel(channel_id);
	if (channel == nullptr)
	{
		return ChannelReceiveResult{ ChannelOpStatus::Closed, std::nullopt };
	}

	ChannelReceiveResult result = channel->Receive(std::move(stop_token));
	EraseIfDrained(channel_id, channel);
	return result;
}

std::optional<SerializedValue> ChannelRegistry::TryReceive(int channel_id)
{
	std::shared_ptr<Channel> channel = FindChannel(channel_id);
	if (channel == nullptr)
	{
		return std::nullopt;
	}

	std::optional<SerializedValue> result = channel->TryReceive();
	EraseIfDrained(channel_id, channel);
	return result;
}

void ChannelRegistry::Close(int channel_id)
{
	std::shared_ptr<Channel> channel = FindChannel(channel_id);
	if (channel != nullptr)
	{
		channel->Close();
		EraseIfDrained(channel_id, channel);
	}
}

size_t ChannelRegistry::GetChannelCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_channels.size();
}

std::shared_ptr<Channel> ChannelRegistry::FindChannel(int channel_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::unordered_map<int, std::shared_ptr<Channel>>::const_iterator channel_it = m_channels.find(channel_id);
	if (channel_it == m_channels.end())
	{
		return nullptr;
	}
	return channel_it->second;
}

// Lock order: registry mutex, then channel mutex (via IsDrained). Channel must never call back into the registry.
void ChannelRegistry::EraseIfDrained(int channel_id, const std::shared_ptr<Channel>& channel)
{
	if (!channel->IsCloseRequested())
	{
		return;
	}

	std::lock_guard<std::mutex> lock(m_mutex);
	std::unordered_map<int, std::shared_ptr<Channel>>::iterator channel_it = m_channels.find(channel_id);
	if (channel_it != m_channels.end() && channel_it->second->IsDrained())
	{
		m_channels.erase(channel_it);
	}
}
