#include "Worker.h"

#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Interpreter/Worker/CooperativeScheduler.h"
#include "Support/Cancellation/Cancellation.h"
#include "Support/Terminal/Terminal.h"

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace
{
	constexpr unsigned s_worker_id_bits = 32u;

	// Installs what crossed at the spawn: the globals the worker can read, then the
	// spawned function and its arguments. A worker used to rebuild its globals
	// instead: it re-ran every imported module's initializer (repeating any effect,
	// such as printing, once per spawn) and restored only the entry module's
	// globals whose names matched a procedure, so data globals read as zero and
	// pointer-valued ones crashed the worker. The payload is taken by value so it
	// is freed once it is in the worker's heap, rather than held beside that copy
	// until the join.
	void InstallPayload(VirtualMachine& worker_vm, TransferPayload payload)
	{
		// The spawn sends the arguments, then the function, as one tuple.
		MarmotTraceable* crossing = ValueTransfer::Deserialize(payload, worker_vm).GetPointer();
		const std::span<const MarmotValue> values(crossing->GetValues(), static_cast<size_t>(crossing->GetLength()));
		worker_vm.PrepareWorkerCall(values.back());
		for (MarmotValue argument : values.first(values.size() - 1uz))
		{
			worker_vm.GetValueStackPointer()[0] = argument;
			worker_vm.AdvanceValueStackPointer();
		}
	}
}

Worker::Worker(MarmotInteger id, size_t lane, std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer)
	: m_id(id)
	, m_lane(lane)
	, m_executable(std::move(executable))
	, m_payload(std::move(payload))
	, m_result_is_pointer(result_is_pointer)
{
}

void Worker::StartOnThread(WorkerRegistry& registry)
{
	m_thread = std::jthread([this, &registry]()
	{
		Resume(registry);
	});
}

Worker::~Worker()
{
	// A worker still running at shutdown is stopped by the runtime, so that
	// cancellation is no failure of its own.
	const bool stopped_here = !IsDone();
	if (m_thread.joinable())
	{
		m_stop_source.request_stop();
		m_thread.join();
	}
	if (IsDone() && !m_outcome.has_value() && !(stopped_here && m_outcome.error().m_cancelled))
	{
		MarmotTerminal::Print<MarmotTerminal::Color::RED>(stderr, std::format("[Worker] Unjoined worker error: {}\n", m_outcome.error().m_message));
	}
}

std::optional<WorkerSuspension> Worker::Resume(WorkerRegistry& registry)
{
	try
	{
		std::optional<Outcome> outcome = RunSlice(registry);
		if (!outcome.has_value())
		{
			return m_vm->m_suspension;
		}
		m_outcome = std::move(*outcome);
	}
	catch (const std::exception& exception)
	{
		m_outcome = std::unexpected(WorkerError{ false, std::string("Unhandled worker exception: ") + exception.what() });
	}
	m_vm.reset();
	m_done.store(true);
	return std::nullopt;
}

std::optional<Worker::Outcome> Worker::RunSlice(WorkerRegistry& registry)
{
	const std::stop_token stop_token = m_stop_source.get_token();
	if (stop_token.stop_requested())
	{
		if (m_vm == nullptr)
		{
			return std::unexpected(WorkerError{ true, "Worker cancelled before execution." });
		}
		// If a channel event selected this worker before it was cancelled,
		// another waiter must get the chance to use the message or space.
		const WorkerWait* event = std::get_if<WorkerWait>(&*m_vm->m_suspension);
		if (event != nullptr && (event->m_kind == WorkerWait::Kind::ChannelSend || event->m_kind == WorkerWait::Kind::ChannelReceive))
		{
			registry.GetScheduler()->WakeOne(*event);
		}
		return std::unexpected(WorkerError{ true, "Worker cancelled." });
	}
	if (m_vm == nullptr)
	{
		const CooperativeScheduler* const scheduler = registry.GetScheduler();
		if (scheduler == nullptr || scheduler->ThreadCount() > 1u)
		{
			const std::expected<void, std::string> safety_check = DynamicFFIRegistry::ValidateWorkerSafety(m_executable->GetNativeLibraries(), registry.GetStdlibWorkerErrorHooks() != nullptr);
			if (!safety_check.has_value())
			{
				return std::unexpected(WorkerError{ false, safety_check.error() });
			}
		}

		m_vm = std::make_unique<VirtualMachine>(m_executable);
		m_vm->m_worker_registry = &registry;
		m_vm->m_scheduled_id = m_id;
		m_vm->SetStopToken(stop_token);
		InstallPayload(*m_vm, std::move(m_payload));
	}

	ThreadCancellation::SetCurrentThreadStopToken(stop_token);
	const VirtualMachine::ExecuteResult execution_result = m_vm->ExecuteSlice();
	ThreadCancellation::SetCurrentThreadStopToken({});
	if (m_vm->m_suspension.has_value())
	{
		return std::nullopt;
	}
	if (!execution_result.has_value())
	{
		return std::unexpected(WorkerError{ execution_result.error().m_code == RuntimeErrorCode::WorkerCancelled, std::string(execution_result.error().m_message) });
	}

	return ValueTransfer::Serialize(m_vm->PeekValue(), m_result_is_pointer, *m_vm)
		.transform_error([](std::string_view message) { return WorkerError{ false, std::string(message) }; });
}

std::expected<TransferPayload, WorkerError> Worker::JoinValue()
{
	if (m_thread.joinable())
	{
		m_thread.join();
	}
	return std::exchange(m_outcome, Outcome{});
}

bool Worker::IsDone() const
{
	return m_done.load();
}

bool Worker::Cancel()
{
	if (m_done.load())
	{
		return false;
	}

	return m_stop_source.request_stop();
}

WorkerRegistry& WorkerRegistry::GetInstance()
{
	static WorkerRegistry instance;
	return instance;
}

std::expected<int, RuntimeError> WorkerRegistry::RunCooperatively(VirtualMachine& main, size_t thread_count)
{
	const std::vector<VmNativeLibraryImport>& libraries = main.GetExecutable().GetNativeLibraries();
	const std::vector<VmNativeLibraryImport>::const_iterator stdlib = std::ranges::find(libraries, "marmotstdlib", &VmNativeLibraryImport::m_name);
	std::optional<StdlibWorkerErrorHooks> hooks;
	if (stdlib != libraries.end() && !stdlib->m_symbols.empty())
	{
		const std::string symbol = stdlib->m_name + VM_NATIVE_SYMBOL_SEPARATOR + stdlib->m_symbols.front();
		if (const VmNativeFunction* function = main.m_dynamic_ffi_registry.FindFunction(symbol))
		{
			hooks = SharedLibraryCache::FindStdlibWorkerErrorHooks(*function);
		}
		if (!hooks.has_value())
		{
			return std::unexpected(RuntimeError
			(
				RuntimeErrorCode::IncompatibleWorkerLibrary,
				"Cooperative workers require a marmotstdlib build that supports per-worker native errors."
			));
		}
	}
	CooperativeScheduler scheduler(thread_count);
	WorkerRegistry registry(&scheduler, hooks ? &*hooks : nullptr);
	constexpr MarmotInteger main_id = 0;
	scheduler.Enqueue(main_id, 0u);
	std::optional<VirtualMachine::ExecuteResult> main_result;
	const auto run_lane = [&](size_t lane)
	{
		while (const std::optional<MarmotInteger> id = scheduler.Next(lane))
		{
			if (*id == main_id)
			{
				WorkerRegistry* const previous_registry = std::exchange(main.m_worker_registry, &registry);
				VirtualMachine::ExecuteResult result = main.ExecuteSlice();
				main.m_worker_registry = previous_registry;
				if (!main.m_suspension.has_value())
				{
					main_result = std::move(result);
					scheduler.Stop();
					break;
				}
				scheduler.FinishSlice(*id, main.m_suspension);
			}
			else
			{
				const std::shared_ptr<Worker> worker = registry.FindWorker(*id);
				scheduler.FinishSlice(*id, worker->Resume(registry));
			}
		}
		registry.ReleaseLaneVMs(lane);
	};

	std::vector<std::jthread> threads;
	try
	{
		for (size_t lane = 1u; lane < thread_count; ++lane)
		{
			threads.emplace_back(run_lane, lane);
		}
	}
	catch (const std::exception& error)
	{
		scheduler.Stop();
		return std::unexpected(RuntimeError(RuntimeErrorCode::WorkerSchedulerStartFailed, std::format("Cannot start scheduler threads: {}.", error.what())));
	}
	run_lane(0u);
	threads.clear();
	if (main_result.has_value())
	{
		return std::move(*main_result);
	}
	return std::unexpected(main.GenerateRuntimeError(RuntimeErrorCode::ConcurrencyDeadlock, "No runnable workers or pending operations can wake the suspended program.", main.GetLine()));
}

std::shared_ptr<Worker> WorkerRegistry::FindWorker(MarmotInteger id)
{
	const std::lock_guard<std::mutex> lock(m_mutex);
	return m_workers.at(id);
}

void WorkerRegistry::ReleaseLaneVMs(size_t lane)
{
	std::vector<std::shared_ptr<Worker>> workers;
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		for (const WorkerMap::value_type& entry : m_workers)
		{
			if (entry.second->m_lane == lane)
			{
				workers.push_back(entry.second);
			}
		}
	}
	// Value buffers must be freed before the allocating thread's pool is destroyed.
	for (const std::shared_ptr<Worker>& worker : workers)
	{
		worker->m_vm.reset();
	}
}

bool WorkerRegistry::Owner::Owns(MarmotInteger handle) const noexcept
{
	return m_id != 0u && (static_cast<MarmotWord>(handle) >> s_worker_id_bits) == m_id;
}

std::optional<MarmotInteger> WorkerRegistry::SpawnWorker(Owner& owner, std::shared_ptr<const VmExecutable> executable, TransferPayload payload, bool result_is_pointer)
{
	MarmotInteger worker_id;
	size_t lane = 0u;
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		if (owner.m_id == 0u)
		{
			if (m_next_owner_id == 0u)
			{
				return std::nullopt;
			}
			owner.m_id = m_next_owner_id++;
		}
		if (owner.m_next_worker_id == 0u)
		{
			return std::nullopt;
		}

		// Keep ownership in the handle so it can be checked after the worker is joined.
		const MarmotWord owner_bits = static_cast<MarmotWord>(owner.m_id) << s_worker_id_bits;
		worker_id = static_cast<MarmotInteger>(owner_bits | owner.m_next_worker_id++);
		if (m_scheduler != nullptr)
		{
			lane = ++m_next_lane % m_scheduler->ThreadCount();
		}
		const std::shared_ptr<Worker>& worker = m_workers.emplace(worker_id,
			std::make_shared<Worker>(worker_id, lane, std::move(executable), std::move(payload), result_is_pointer)).first->second;
		if (m_scheduler == nullptr)
		{
			worker->StartOnThread(*this);
		}
	}
	if (m_scheduler != nullptr)
	{
		m_scheduler->Enqueue(worker_id, lane);
	}
	return worker_id;
}

std::optional<std::expected<TransferPayload, WorkerError>> WorkerRegistry::JoinWorkerValue(MarmotInteger worker_id, MarmotInteger caller, const std::stop_token& stop)
{
	std::shared_ptr<Worker> worker;
	{
		std::unique_lock<std::mutex> scheduler_lock;
		if (m_scheduler != nullptr)
		{
			scheduler_lock = std::unique_lock(m_scheduler->m_mutex);
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		const WorkerMap::iterator worker_it = m_workers.find(worker_id);
		if (worker_it == m_workers.end())
		{
			// A missing handle owned by the caller has already been joined.
			return std::unexpected(WorkerError{ false, "This worker was already joined; its result was returned to the first join." });
		}
		if (m_scheduler != nullptr && !worker_it->second->IsDone())
		{
			m_scheduler->ParkLocked(caller, WorkerWait{ WorkerWait::Kind::Join, worker_id }, stop);
			return std::nullopt;
		}

		worker = std::move(worker_it->second);
		m_workers.erase(worker_it);
	}
	return worker->JoinValue();
}

bool WorkerRegistry::IsWorkerDone(MarmotInteger worker_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto worker_it = m_workers.find(worker_id);
	if (worker_it == m_workers.end())
	{
		return true;
	}
	return worker_it->second->IsDone();
}

bool WorkerRegistry::CancelWorker(MarmotInteger worker_id)
{
	bool cancelled;
	{
		const std::lock_guard<std::mutex> lock(m_mutex);
		const WorkerMap::iterator worker = m_workers.find(worker_id);
		if (worker == m_workers.end())
		{
			return false;
		}
		cancelled = worker->second->Cancel();
	}
	if (cancelled && m_scheduler != nullptr)
	{
		m_scheduler->WakeWorker(worker_id);
	}
	return cancelled;
}

void WorkerRegistry::Shutdown()
{
	WorkerMap pending_workers;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		pending_workers = std::move(m_workers);
		m_workers.clear();
	}
}
