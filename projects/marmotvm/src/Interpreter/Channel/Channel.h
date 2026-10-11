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
	std::optional<TransferPayload> m_value = std::nullopt;
};

class Channel
{
public:
	explicit Channel(int capacity);

	ChannelOpStatus Send(TransferPayload message, std::stop_token stop_token);

	ChannelReceiveResult Receive(std::stop_token stop_token);

	void Close();

	bool IsDrained() const;

	bool IsClosed() const noexcept;

private:
	mutable std::mutex m_mutex;
	std::condition_variable_any m_not_empty;
	std::condition_variable_any m_not_full;
	std::deque<TransferPayload> m_queue;
	int m_capacity;
	// Written under m_mutex, so the waits see it, and read without it so the
	// registry is locked only for a channel that may need reclaiming.
	std::atomic<bool> m_closed{ false };
};

class ChannelRegistry
{
public:
	static ChannelRegistry& GetInstance();

	int CreateChannel(int capacity);

	ChannelOpStatus Send(int channel_id, TransferPayload message, std::stop_token stop_token);

	ChannelReceiveResult Receive(int channel_id, std::stop_token stop_token);

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
