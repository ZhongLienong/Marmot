#pragma once

#include "Interpreter/ValueTransfer/ValueTransfer.h"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class VmExecutable;

// Why a worker has no value. A join tells cancellation apart from the rest, which
// reach the joining code as their message.
struct WorkerError
{
	bool m_cancelled = false;
	std::string m_message;
};

class Worker
{
public:
	Worker(std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer);

	~Worker();

	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	Worker(Worker&&) = delete;
	Worker& operator=(Worker&&) = delete;

	std::expected<TransferPayload, WorkerError> JoinValue();

	bool IsDone() const;

	bool Cancel();

private:
	void Execute(std::stop_token stop_token);

	std::expected<TransferPayload, WorkerError> Run(std::stop_token stop_token);

	std::shared_ptr<const VmExecutable> m_executable;
	// The spawned function and its arguments, as one tuple, with the globals they
	// can read.
	TransferPayload m_payload;
	bool m_result_is_pointer;
	// Written by the worker thread, read only once it has been joined.
	std::expected<TransferPayload, WorkerError> m_outcome;
	std::atomic<bool> m_done{ false };
	// Last, so it starts once everything it reads is built.
	std::jthread m_thread;
};

class WorkerRegistry
{
public:
	class Owner
	{
	public:
		Owner() = default;
		Owner(const Owner&) = delete;
		Owner& operator=(const Owner&) = delete;

		bool Owns(MarmotInteger handle) const noexcept;

	private:
		friend class WorkerRegistry;

		uint32_t m_id = 0u;
		uint32_t m_next_worker_id = 1u;
	};

	static WorkerRegistry& GetInstance();

	std::optional<MarmotInteger> SpawnWorker(Owner& owner, std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer);

	std::expected<TransferPayload, WorkerError> JoinWorkerValue(MarmotInteger worker_id);

	bool IsWorkerDone(MarmotInteger worker_id) const;

	bool CancelWorker(MarmotInteger worker_id);

	void Shutdown();

private:
	WorkerRegistry() = default;

	mutable std::mutex m_mutex;
	std::unordered_map<MarmotInteger, std::unique_ptr<Worker>> m_workers;
	uint32_t m_next_owner_id = 1u;
};
