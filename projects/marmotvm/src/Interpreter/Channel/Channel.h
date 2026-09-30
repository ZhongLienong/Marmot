#pragma once

#include "Interpreter/ValueTransfer/ValueTransfer.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <unordered_map>

enum class ChannelOpStatus
{
	Ok,
	Closed,
	Cancelled
};

struct ChannelReceiveResult
{
	ChannelOpStatus m_status = ChannelOpStatus::Closed;
	std::optional<SerializedValue> m_value = std::nullopt;
};

class Channel
{
public:
	explicit Channel(int capacity);

	ChannelOpStatus Send(SerializedValue message, std::stop_token stop_token);

	ChannelReceiveResult Receive(std::stop_token stop_token);

	std::optional<SerializedValue> TryReceive();

	void Close();

	bool IsClosed() const;

	bool IsDrained() const;

	bool IsCloseRequested() const noexcept;

private:
	mutable std::mutex m_mutex;
	std::condition_variable_any m_not_empty;
	std::condition_variable_any m_not_full;
	std::deque<SerializedValue> m_queue;
	int m_capacity;
	bool m_closed = false;
	std::atomic<bool> m_close_requested{ false };
};

class ChannelRegistry
{
public:
	static ChannelRegistry& GetInstance();

	int CreateChannel(int capacity);

	ChannelOpStatus Send(int channel_id, SerializedValue message, std::stop_token stop_token);

	ChannelReceiveResult Receive(int channel_id, std::stop_token stop_token);

	std::optional<SerializedValue> TryReceive(int channel_id);

	void Close(int channel_id);

	size_t GetChannelCount() const;

private:
	ChannelRegistry() = default;

	mutable std::mutex m_mutex;
	std::unordered_map<int, std::shared_ptr<Channel>> m_channels;
	int m_next_id = 1;

	std::shared_ptr<Channel> FindChannel(int channel_id) const;

	void EraseIfDrained(int channel_id, const std::shared_ptr<Channel>& channel);
};
