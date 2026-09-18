#pragma once

#include "Common/Error/Error.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct WorkerError
{
	RuntimeErrorCode m_code = RuntimeErrorCode::InternalTypeError;
	std::string m_message;
};

class Worker
{
public:
	Worker(std::shared_ptr<const MidoriExecutable> executable, SerializedValue serialized_function, std::vector<SerializedValue> serialized_args, std::vector<SerializedValue> serialized_globals);

	~Worker();

	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	Worker(Worker&&) = delete;
	Worker& operator=(Worker&&) = delete;

	std::expected<SerializedValue, WorkerError> JoinValue();



	bool IsDone() const;

	bool Cancel();

private:
	void Execute(std::stop_token stop_token);

	std::jthread m_thread;
	std::shared_ptr<const MidoriExecutable> m_executable;
	// The spawned function, transferred as a value: its procedure index and a copy
	// of its captured cells, deserialized into the worker's own heap.
	SerializedValue m_serialized_function;
	std::vector<SerializedValue> m_serialized_args;
	std::vector<SerializedValue> m_serialized_globals;
	std::atomic<bool> m_done{ false };
	std::atomic<bool> m_joined{ false };
	std::mutex m_result_mutex;
	std::optional<SerializedValue> m_result;
	std::string m_error;
	RuntimeErrorCode m_error_code = RuntimeErrorCode::InternalTypeError;
	int m_exit_code = 0;
	bool m_had_error = false;
};

class WorkerRegistry
{
public:
	static WorkerRegistry& GetInstance();

	int SpawnWorker(std::shared_ptr<const MidoriExecutable> executable, SerializedValue serialized_function, std::vector<SerializedValue> serialized_args, std::vector<SerializedValue> serialized_globals);

	std::expected<SerializedValue, WorkerError> JoinWorkerValue(int worker_id);

	bool IsWorkerDone(int worker_id) const;

	bool CancelWorker(int worker_id);

	void Shutdown();

private:
	WorkerRegistry() = default;

	mutable std::mutex m_mutex;
	std::unordered_map<int, std::unique_ptr<Worker>> m_workers;
	int m_next_id = 1;
};
