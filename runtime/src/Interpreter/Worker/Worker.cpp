#include "Worker.h"

#include "Common/Cancellation/Cancellation.h"
#include "Common/Printer/Printer.h"

#include <unordered_map>

namespace
{
	// Installs the spawning VM's globals, copied at spawn time. A worker used to
	// rebuild its globals instead: it re-ran every imported module's initializer
	// (repeating any effect, such as printing, once per spawn) and restored only
	// the entry module's globals whose names matched a procedure, so data globals
	// read as zero and pointer-valued ones crashed the worker.
	std::expected<void, WorkerError> InstallWorkerGlobals(VirtualMachine& worker_vm, const std::vector<SerializedValue>& serialized_globals, ValueTransfer::DeserializePointerMap& sharing)
	{
		for (size_t global_index = 0uz; global_index < serialized_globals.size(); global_index += 1uz)
		{
			std::expected<MidoriValue, std::string> global_value = ValueTransfer::Deserialize(serialized_globals[global_index], worker_vm, sharing);
			if (!global_value.has_value())
			{
				return std::unexpected(WorkerError{ RuntimeErrorCode::InternalTypeError, global_value.error() });
			}
			worker_vm.SetGlobalValue(static_cast<int>(global_index), global_value.value());
		}

		return {};
	}
}

Worker::Worker(std::shared_ptr<const MidoriExecutable> executable, SerializedValue serialized_function, std::vector<SerializedValue> serialized_args, std::vector<SerializedValue> serialized_globals)
	: m_executable(std::move(executable))
	, m_serialized_function(std::move(serialized_function))
	, m_serialized_args(std::move(serialized_args))
	, m_serialized_globals(std::move(serialized_globals))
{
	m_thread = std::jthread([this](std::stop_token stop_token)
	{
		Execute(std::move(stop_token));
	});
}

Worker::~Worker()
{
	if (!m_joined.load())
	{
		if (m_thread.joinable())
		{
			m_thread.request_stop();
			m_thread.join();
		}

		if (m_had_error)
		{
			std::lock_guard<std::mutex> lock(m_result_mutex);
			Printer::PrintFormatted<Printer::Color::RED>("[Worker] Unjoined worker error: {}\n", m_error);
		}
	}
}

void Worker::Execute(std::stop_token stop_token)
{
	try
	{
		if (stop_token.stop_requested())
		{
			std::lock_guard<std::mutex> lock(m_result_mutex);
			m_error = "Worker cancelled before execution.";
			m_error_code = RuntimeErrorCode::WorkerCancelled;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		VirtualMachine worker_vm(m_executable, 0, nullptr);
		worker_vm.SetStopToken(stop_token);
		ThreadCancellation::SetCurrentThreadStopToken(stop_token);

		std::expected<void, std::string> safety_check = worker_vm.GetDynamicFFIRegistry().ValidateWorkerSafety();
		if (!safety_check.has_value())
		{
			std::lock_guard<std::mutex> lock(m_result_mutex);
			m_error = safety_check.error();
			m_error_code = RuntimeErrorCode::InternalTypeError;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		// One sharing map for the whole payload, matching how SPAWN_WORKER serialised it.
		ValueTransfer::DeserializePointerMap sharing;
		std::expected<void, WorkerError> init_result = InstallWorkerGlobals(worker_vm, m_serialized_globals, sharing);
		if (!init_result.has_value())
		{
			std::lock_guard<std::mutex> lock(m_result_mutex);
			m_error = init_result.error().m_message;
			m_error_code = init_result.error().m_code;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		std::expected<MidoriValue, std::string> worker_function = ValueTransfer::Deserialize(m_serialized_function, worker_vm, sharing);
		if (!worker_function.has_value())
		{
			std::lock_guard<std::mutex> lock(m_result_mutex);
			m_error = worker_function.error();
			m_error_code = RuntimeErrorCode::InternalTypeError;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		worker_vm.PrepareWorkerCall(worker_function.value());
		for (const SerializedValue& serialized_arg : m_serialized_args)
		{
			std::expected<MidoriValue, std::string> deserialized_arg = ValueTransfer::Deserialize(serialized_arg, worker_vm, sharing);
			if (!deserialized_arg.has_value())
			{
				std::lock_guard<std::mutex> lock(m_result_mutex);
				m_error = deserialized_arg.error();
				m_error_code = RuntimeErrorCode::InternalTypeError;
				m_had_error = true;
				m_done.store(true);
				return;
			}

			worker_vm.GetValueStackPointer()[0] = deserialized_arg.value();
			worker_vm.AdvanceValueStackPointer();
		}

		VirtualMachine::ExecuteResult execution_result = worker_vm.Execute();
		std::lock_guard<std::mutex> lock(m_result_mutex);
		if (!execution_result.has_value())
		{
			m_error = std::string(execution_result.error().m_message);
			m_error_code = execution_result.error().m_code;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		m_exit_code = execution_result.value();
		std::expected<SerializedValue, std::string> serialized_result = ValueTransfer::Serialize(worker_vm.PeekValue(), worker_vm);
		if (!serialized_result.has_value())
		{
			m_error = serialized_result.error();
			m_error_code = RuntimeErrorCode::InternalTypeError;
			m_had_error = true;
			m_done.store(true);
			return;
		}

		m_result = std::move(serialized_result.value());
		m_had_error = false;
		m_done.store(true);
	}
	catch (const std::exception& exception)
	{
		std::lock_guard<std::mutex> lock(m_result_mutex);
		m_error = std::string("Unhandled worker exception: ") + exception.what();
		m_error_code = RuntimeErrorCode::InternalTypeError;
		m_had_error = true;
		m_done.store(true);
	}
	catch (...)
	{
		std::lock_guard<std::mutex> lock(m_result_mutex);
		m_error = "Unhandled worker exception.";
		m_error_code = RuntimeErrorCode::InternalTypeError;
		m_had_error = true;
		m_done.store(true);
	}
}

std::expected<SerializedValue, WorkerError> Worker::JoinValue()
{
	m_joined.store(true);
	if (m_thread.joinable())
	{
		m_thread.join();
	}

	std::lock_guard<std::mutex> lock(m_result_mutex);
	if (m_had_error)
	{
		return std::unexpected(WorkerError{ m_error_code, m_error });
	}
	if (!m_result.has_value())
	{
		return std::unexpected(WorkerError{ RuntimeErrorCode::InternalTypeError, std::string("Worker completed without a result.") });
	}
	return m_result.value();
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

int WorkerRegistry::SpawnWorker(std::shared_ptr<const MidoriExecutable> executable, SerializedValue serialized_function, std::vector<SerializedValue> serialized_args, std::vector<SerializedValue> serialized_globals)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const int worker_id = m_next_id;
	m_next_id += 1;
	m_workers.emplace(worker_id, std::make_unique<Worker>(std::move(executable), std::move(serialized_function), std::move(serialized_args), std::move(serialized_globals)));
	return worker_id;
}

std::expected<SerializedValue, WorkerError> WorkerRegistry::JoinWorkerValue(int worker_id)
{
	std::unique_ptr<Worker> worker;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::unordered_map<int, std::unique_ptr<Worker>>::iterator worker_it = m_workers.find(worker_id);
		if (worker_it == m_workers.end())
		{
			return std::unexpected(WorkerError{ RuntimeErrorCode::InternalTypeError, "Worker not found: " + std::to_string(worker_id) });
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
