#include "Worker.h"

#include "Support/Cancellation/Cancellation.h"
#include "Support/Terminal/Terminal.h"

#include <span>
#include <unordered_map>

namespace
{
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
		MidoriTraceable* crossing = ValueTransfer::Deserialize(payload, worker_vm).GetPointer();
		const std::span<const MidoriValue> values(crossing->GetValues(), static_cast<size_t>(crossing->GetLength()));
		worker_vm.PrepareWorkerCall(values.back());
		for (MidoriValue argument : values.first(values.size() - 1uz))
		{
			worker_vm.GetValueStackPointer()[0] = argument;
			worker_vm.AdvanceValueStackPointer();
		}
	}
}

Worker::Worker(std::shared_ptr<const VmExecutable> executable, TransferPayload payload)
	: m_executable(std::move(executable))
	, m_payload(std::move(payload))
	, m_thread([this](std::stop_token stop_token)
	{
		Execute(std::move(stop_token));
	})
{
}

Worker::~Worker()
{
	if (m_thread.joinable())
	{
		m_thread.request_stop();
		m_thread.join();
		if (!m_outcome.has_value())
		{
			RuntimeTerminal::Print<RuntimeTerminal::Color::RED>(stderr, std::format("[Worker] Unjoined worker error: {}\n", m_outcome.error().m_message));
		}
	}
}

void Worker::Execute(std::stop_token stop_token)
{
	try
	{
		m_outcome = Run(std::move(stop_token));
	}
	catch (const std::exception& exception)
	{
		m_outcome = std::unexpected(WorkerError{ false, std::string("Unhandled worker exception: ") + exception.what() });
	}
	m_done.store(true);
}

std::expected<TransferPayload, WorkerError> Worker::Run(std::stop_token stop_token)
{
	if (stop_token.stop_requested())
	{
		return std::unexpected(WorkerError{ true, "Worker cancelled before execution." });
	}

	VirtualMachine worker_vm(m_executable);
	worker_vm.SetStopToken(stop_token);
	ThreadCancellation::SetCurrentThreadStopToken(stop_token);

	std::expected<void, std::string> safety_check = DynamicFFIRegistry::ValidateWorkerSafety(m_executable->GetNativeLibraries());
	if (!safety_check.has_value())
	{
		return std::unexpected(WorkerError{ false, safety_check.error() });
	}

	InstallPayload(worker_vm, std::move(m_payload));
	VirtualMachine::ExecuteResult execution_result = worker_vm.Execute();
	if (!execution_result.has_value())
	{
		return std::unexpected(WorkerError{ execution_result.error().m_code == RuntimeErrorCode::WorkerCancelled, std::string(execution_result.error().m_message) });
	}

	return ValueTransfer::Serialize(worker_vm.PeekValue(), worker_vm);
}

std::expected<TransferPayload, WorkerError> Worker::JoinValue()
{
	m_thread.join();
	return std::move(m_outcome);
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

	return m_thread.request_stop();
}

WorkerRegistry& WorkerRegistry::GetInstance()
{
	static WorkerRegistry instance;
	return instance;
}

int WorkerRegistry::SpawnWorker(std::shared_ptr<const VmExecutable> executable, TransferPayload payload)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const int worker_id = m_next_id;
	m_next_id += 1;
	m_workers.emplace(worker_id, std::make_unique<Worker>(std::move(executable), std::move(payload)));
	return worker_id;
}

std::expected<TransferPayload, WorkerError> WorkerRegistry::JoinWorkerValue(int worker_id)
{
	std::unique_ptr<Worker> worker;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::unordered_map<int, std::unique_ptr<Worker>>::iterator worker_it = m_workers.find(worker_id);
		if (worker_it == m_workers.end())
		{
			// A worker leaves the registry when it is joined, and a handle only
			// ever comes from a spawn, so this is a second join of the same one.
			return std::unexpected(WorkerError{ false, "This worker was already joined; its result was returned to the first join." });
		}

		worker = std::move(worker_it->second);
		m_workers.erase(worker_it);
	}

	return worker->JoinValue();
}

bool WorkerRegistry::IsWorkerDone(int worker_id) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::unordered_map<int, std::unique_ptr<Worker>>::const_iterator worker_it = m_workers.find(worker_id);
	if (worker_it == m_workers.end())
	{
		return true;
	}
	return worker_it->second->IsDone();
}

bool WorkerRegistry::CancelWorker(int worker_id)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::unordered_map<int, std::unique_ptr<Worker>>::iterator worker_it = m_workers.find(worker_id);
	if (worker_it == m_workers.end())
	{
		return false;
	}
	return worker_it->second->Cancel();
}

void WorkerRegistry::Shutdown()
{
	std::unordered_map<int, std::unique_ptr<Worker>> pending_workers;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		pending_workers = std::move(m_workers);
		m_workers.clear();
	}
}
