#pragma once

#include "Error/RuntimeError.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
	static WorkerRegistry& GetInstance();

	int SpawnWorker(std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer);

	std::expected<TransferPayload, WorkerError> JoinWorkerValue(int worker_id);

	bool IsWorkerDone(int worker_id) const;

	bool CancelWorker(int worker_id);

	void Shutdown();

private:
	WorkerRegistry() = default;

	mutable std::mutex m_mutex;
	std::unordered_map<int, std::unique_ptr<Worker>> m_workers;
	int m_next_id = 1;
};
