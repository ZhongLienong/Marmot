#pragma once

#include "Error/RuntimeError.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"

#include <atomic>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>

class CooperativeScheduler;
class VmExecutable;
class VirtualMachine;
class WorkerRegistry;
struct StdlibWorkerErrorHooks;

struct WorkerWait
{
	enum class Kind
	{
		Join,
		ChannelSend,
		ChannelReceive,
		NativeProcess
	};

	Kind m_kind;
	MarmotInteger m_target;

	std::strong_ordering operator<=>(const WorkerWait&) const = default;
};

struct WorkerYield
{
	std::strong_ordering operator<=>(const WorkerYield&) const = default;
};

using WorkerSuspension = std::variant<std::chrono::steady_clock::time_point, WorkerWait, WorkerYield>;

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
	Worker(MarmotInteger id, size_t lane, std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer);

	~Worker();

	Worker(const Worker&) = delete;
	Worker& operator=(const Worker&) = delete;
	Worker(Worker&&) = delete;
	Worker& operator=(Worker&&) = delete;

	std::expected<TransferPayload, WorkerError> JoinValue();

	bool IsDone() const;

	bool Cancel();

private:
	friend class WorkerRegistry;

	using Outcome = std::expected<TransferPayload, WorkerError>;

	void StartOnThread(WorkerRegistry& registry);
	std::optional<WorkerSuspension> Resume(WorkerRegistry& registry);
	std::optional<Outcome> RunSlice(WorkerRegistry& registry);

	const MarmotInteger m_id;
	const size_t m_lane;
	std::shared_ptr<const VmExecutable> m_executable;
	// The spawned function and its arguments, as one tuple, with the globals they
	// can read.
	TransferPayload m_payload;
	bool m_result_is_pointer;
	std::unique_ptr<VirtualMachine> m_vm;
	// Published when m_done is set, consumed once by JoinValue.
	Outcome m_outcome;
	std::atomic<bool> m_done{ false };
	std::stop_source m_stop_source;
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

	// The calling thread runs the main VM and counts toward the thread budget.
	static std::expected<int, RuntimeError> RunCooperatively(VirtualMachine& main, size_t thread_count = 1u);
	CooperativeScheduler* GetScheduler() const noexcept { return m_scheduler; }
	const StdlibWorkerErrorHooks* GetStdlibWorkerErrorHooks() const noexcept { return m_stdlib_worker_error_hooks; }

	std::optional<MarmotInteger> SpawnWorker(Owner& owner, std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer);

	std::optional<std::expected<TransferPayload, WorkerError>> JoinWorkerValue(MarmotInteger worker_id, MarmotInteger caller, const std::stop_token& stop);

	bool IsWorkerDone(MarmotInteger worker_id) const;

	bool CancelWorker(MarmotInteger worker_id);

	void Shutdown();

private:
	explicit WorkerRegistry(CooperativeScheduler* scheduler = nullptr, const StdlibWorkerErrorHooks* hooks = nullptr)
		: m_scheduler(scheduler), m_stdlib_worker_error_hooks(hooks) {}

	std::shared_ptr<Worker> FindWorker(MarmotInteger id);
	void ReleaseLaneVMs(size_t lane);

	using WorkerMap = std::unordered_map<MarmotInteger, std::shared_ptr<Worker>>;

	mutable std::mutex m_mutex;
	WorkerMap m_workers;
	CooperativeScheduler* m_scheduler;
	const StdlibWorkerErrorHooks* m_stdlib_worker_error_hooks;
	uint32_t m_next_owner_id = 1u;
	size_t m_next_lane = 0u;
};
