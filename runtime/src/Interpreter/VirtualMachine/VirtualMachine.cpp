#include "Common/Constant/Constant.h"
#include "Common/BuildConfig/BuildConfig.h"
#include "Common/Printer/Printer.h"
#include "Interpreter/Channel/Channel.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"
#include "Interpreter/Worker/Worker.h"
#include "Utility/Disassembler/Disassembler.h"
#include "VirtualMachine.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif



#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execution>
#include <fstream>
#include <format>
#include <numeric>
#include <ranges>
#include <unordered_map>

using namespace std::string_literals;

#if defined(_MSC_VER)
#define MIDORI_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MIDORI_NOINLINE __attribute__((noinline))
#else
#define MIDORI_NOINLINE
#endif

namespace
{
#if !defined(_WIN32) && !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

	struct ProcedureDisplayName
	{
		std::string m_display_name;
		std::string m_module_name;
		bool m_is_module_bootstrap = false;
	};

	class SourceLineCache
	{
	public:
		explicit SourceLineCache(const MidoriExecutable& executable)
			: m_executable(executable)
		{
		}

		[[nodiscard]] std::optional<std::string> GetLine(std::string_view file_name, int line)
		{
			if (file_name.empty() || line <= 0)
			{
				return std::nullopt;
			}

			const std::string cache_key(file_name);
			std::unordered_map<std::string, std::vector<std::string>>::iterator cache_it = m_source_lines.find(cache_key);
			if (cache_it == m_source_lines.end())
			{
				const std::vector<std::string>* embedded_lines = m_executable.FindSourceLines(cache_key);
				if (embedded_lines != nullptr)
				{
					cache_it = m_source_lines.emplace(cache_key, *embedded_lines).first;
				}
				else
				{
					cache_it = m_source_lines.emplace(cache_key, LoadSourceLines(cache_key)).first;
				}
			}

			const std::vector<std::string>& lines = cache_it->second;
			if (static_cast<size_t>(line) > lines.size())
			{
				return std::nullopt;
			}

			return lines[static_cast<size_t>(line - 1)];
		}

	private:
		const MidoriExecutable& m_executable;
		static std::vector<std::string> LoadSourceLines(const std::string& file_name)
		{
			std::ifstream input(file_name);
			if (!input)
			{
				return {};
			}

			std::vector<std::string> lines;
			std::string line;
			while (std::getline(input, line))
			{
				lines.emplace_back(std::move(line));
			}

			return lines;
		}

		std::unordered_map<std::string, std::vector<std::string>> m_source_lines;
	};

	ProcedureDisplayName ParseProcedureName(const std::string& raw_name) noexcept
	{
		const std::string_view raw_view(raw_name);
		const size_t separator_index = raw_view.rfind(ModuleSeparator);

		std::string_view display_name = raw_view;
		std::string_view module_name;
		if (separator_index != std::string_view::npos)
		{
			display_name = raw_view.substr(0u, separator_index);
			module_name = raw_view.substr(separator_index + 1u);
		}

		const bool is_module_bootstrap = display_name.starts_with(MODULE_BOOTSTRAP_PREFIX);
		if (display_name.starts_with(MAIN_PROCEDURE_PREFIX))
		{
			display_name = "main";
		}
		else if (display_name.empty())
		{
			display_name = ANONYMOUS_FUNCTION;
		}

		return ProcedureDisplayName{ std::string(display_name), std::string(module_name), is_module_bootstrap };
	}

	std::optional<RuntimeStackFrame> ResolveStackTraceFrame(const MidoriExecutable& executable, int proc_index, int line, SourceLineCache& source_line_cache)
	{
		RuntimeStackFrame frame;
		frame.m_location.m_line = line;

		if (proc_index >= 0 && proc_index < executable.GetProcedureCount() && proc_index < static_cast<int>(executable.m_procedure_names.size()))
		{
			const ProcedureDisplayName display_name = ParseProcedureName(executable.m_procedure_names[static_cast<size_t>(proc_index)]);
			if (display_name.m_is_module_bootstrap)
			{
				return std::nullopt;
			}

			frame.m_procedure_name = display_name.m_display_name;
			frame.m_module_name = display_name.m_module_name;
			frame.m_location.m_file_name = std::string(executable.GetProcedureSourcePath(proc_index));
		}
		else
		{
			frame.m_procedure_name = ANONYMOUS_FUNCTION;
			frame.m_location.m_file_name = std::string(executable.GetFileName());
		}

		if (frame.m_location.m_file_name.empty())
		{
			frame.m_location.m_file_name = std::string(executable.GetFileName());
		}

		frame.m_location.m_source_line = source_line_cache.GetLine(frame.m_location.m_file_name, frame.m_location.m_line);
		if (frame.m_location.m_line > 0)
		{
			frame.m_location.m_end_line = frame.m_location.m_line;
		}
		return frame;
	}

	bool CanCollapseRecursiveFrame(const RuntimeStackFrame& left, const RuntimeStackFrame& right) noexcept
	{
		return left.m_procedure_name == right.m_procedure_name
			&& left.m_module_name == right.m_module_name
			&& left.m_location.m_file_name == right.m_location.m_file_name
			&& left.m_location.m_line == right.m_location.m_line;
	}

#ifndef _WIN32
	struct UnixSignalInfo
	{
		int m_signal_number = 0;
		uintptr_t m_fault_address = 0u;
	};

	struct UnixSignalHandlerState
	{
		sigjmp_buf* m_jump_buffer = nullptr;
		UnixSignalInfo* m_signal_info = nullptr;
		struct sigaction m_previous_sigsegv {};
		struct sigaction m_previous_sigfpe {};
#if defined(SIGBUS)
		struct sigaction m_previous_sigbus {};
#endif
	};

	thread_local UnixSignalHandlerState* s_active_unix_signal_handler = nullptr;

	void HandleVirtualMachineSignal(int signal_number, siginfo_t* signal_info, void*)
	{
		if (s_active_unix_signal_handler == nullptr
			|| s_active_unix_signal_handler->m_jump_buffer == nullptr
			|| s_active_unix_signal_handler->m_signal_info == nullptr)
		{
			std::_Exit(128 + signal_number);
		}

		s_active_unix_signal_handler->m_signal_info->m_signal_number = signal_number;
		s_active_unix_signal_handler->m_signal_info->m_fault_address =
			signal_info != nullptr ? reinterpret_cast<uintptr_t>(signal_info->si_addr) : 0u;
		siglongjmp(*s_active_unix_signal_handler->m_jump_buffer, 1);
	}

	bool InstallVirtualMachineSignalHandlers(UnixSignalHandlerState& handler_state)
	{
		struct sigaction action {};
		std::memset(&action, 0, sizeof(action));
		sigemptyset(&action.sa_mask);
		action.sa_sigaction = HandleVirtualMachineSignal;
		action.sa_flags = SA_SIGINFO;

		if (sigaction(SIGSEGV, &action, &handler_state.m_previous_sigsegv) != 0)
		{
			return false;
		}

		if (sigaction(SIGFPE, &action, &handler_state.m_previous_sigfpe) != 0)
		{
			static_cast<void>(sigaction(SIGSEGV, &handler_state.m_previous_sigsegv, nullptr));
			return false;
		}

#if defined(SIGBUS)
		if (sigaction(SIGBUS, &action, &handler_state.m_previous_sigbus) != 0)
		{
			static_cast<void>(sigaction(SIGFPE, &handler_state.m_previous_sigfpe, nullptr));
			static_cast<void>(sigaction(SIGSEGV, &handler_state.m_previous_sigsegv, nullptr));
			return false;
		}
#endif

		return true;
	}

	void RestoreVirtualMachineSignalHandlers(const UnixSignalHandlerState& handler_state)
	{
		static_cast<void>(sigaction(SIGSEGV, &handler_state.m_previous_sigsegv, nullptr));
		static_cast<void>(sigaction(SIGFPE, &handler_state.m_previous_sigfpe, nullptr));
#if defined(SIGBUS)
		static_cast<void>(sigaction(SIGBUS, &handler_state.m_previous_sigbus, nullptr));
#endif
	}
#endif
}

VirtualMachine::VirtualMachine(MidoriExecutable&& executable) noexcept
	: m_owned_executable(std::make_shared<MidoriExecutable>(std::move(executable)))
{
	m_gc.SetAllocator(&m_allocator);
	m_executable = m_owned_executable.get();
	m_owned_globals.resize(static_cast<size_t>(m_executable->GetGlobalVariableCount()));
	m_global_vars = &m_owned_globals;
	m_string_literal_cache.resize(m_executable->GetStringPool().size(), nullptr);

	InitializeProcEntryCache();
	InitializeStacks();

	constexpr int runtime_startup_proc_index = 0;
	m_instruction_pointer = GetProcEntry(runtime_startup_proc_index);
}

namespace
{
	static constexpr OpCode s_halt_bytecode[] = { OpCode::HALT };
}

VirtualMachine::VirtualMachine(std::shared_ptr<const MidoriExecutable> shared_executable, int proc_index, const GlobalVariables* source_globals) noexcept
	: m_owned_executable(std::move(shared_executable))
	, m_worker_proc_index(proc_index)
{
	m_gc.SetAllocator(&m_allocator);
	m_executable = m_owned_executable.get();

	if (source_globals != nullptr)
	{
		m_owned_globals = *source_globals;
	}
	else
	{
		m_owned_globals.resize(static_cast<size_t>(m_executable->GetGlobalVariableCount()));
	}
	m_global_vars = &m_owned_globals;
	m_string_literal_cache.resize(m_executable->GetStringPool().size(), nullptr);

	InitializeProcEntryCache();
	InitializeStacks();

	m_instruction_pointer = GetProcEntry(proc_index);

	PushCallFrame(m_value_stack_begin, &s_halt_bytecode[0], nullptr);
	m_value_stack_base_pointer = m_value_stack_pointer;
}

void VirtualMachine::PrepareWorkerCall(MidoriValue worker_function) noexcept
{
	m_value_stack_pointer = m_value_stack_begin;
	m_value_stack_base_pointer = m_value_stack_begin;
	m_call_stack_pointer = m_call_stack_begin;
	m_last_error.reset();
	m_is_worker = true;

	// The spawned function is a transferred closure rather than a procedure index:
	// it is not on this VM's stack, so it is rooted here, and its cells are the
	// environment the worker's first frame runs in - which is what lets a lambda
	// that captured something be spawned.
	MidoriTraceable* closure_pointer = worker_function.GetPointer();
	MidoriClosure& closure = closure_pointer->GetTraceable<MidoriClosure>();
	m_curr_closure_traceable = closure_pointer;
	m_curr_environment = &closure.m_cell_values;

	m_instruction_pointer = GetProcEntry(closure.m_proc_index);

	PushCallFrame(m_value_stack_begin, &s_halt_bytecode[0], m_curr_environment);
	m_value_stack_base_pointer = m_value_stack_pointer;
}

MidoriValue VirtualMachine::MakeFunctionValue(int proc_index) noexcept
{
	const size_t cache_index = static_cast<size_t>(proc_index);
	if (cache_index < m_static_closure_cache.size() && m_static_closure_cache[cache_index] != nullptr)
	{
		return m_static_closure_cache[cache_index];
	}

	MidoriTraceable* closure = AllocateTraceable(MidoriClosure{ .m_cell_values = MidoriTuple(), .m_proc_index = proc_index });
	if (cache_index < m_static_closure_cache.size())
	{
		m_static_closure_cache[cache_index] = closure;
	}

	return closure;
}

MIDORI_NOINLINE bool VirtualMachine::ExecuteConcurrencyInstruction(OpCode instruction, InstructionPointer& ip) noexcept
{
#ifdef MIDORI_WASM
	m_instruction_pointer = ip;
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::UnsupportedPlatformOperation, "Concurrency is not supported in the WebAssembly build.", GetLine())));
	return false;
#else
	switch (instruction)
	{
	case OpCode::SPAWN_WORKER:
	{
		int arg_count = static_cast<int>(ReadByte(ip));

		// The function is an ordinary value on the stack, above its arguments, and
		// crosses to the worker like any other transferred value: its procedure index
		// plus a copy of whatever cells it captured.
		MidoriValue worker_function = Pop();
		// The function, its arguments and the globals cross as one payload through one
		// sharing map, so an object reached twice (a cell passed twice, or captured and
		// also passed) arrives as one copy rather than two.
		ValueTransfer::SerializePointerMap sharing;
		std::expected<SerializedValue, std::string> serialized_function = ValueTransfer::Serialize(worker_function, *this, sharing);
		if (!serialized_function.has_value())
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, serialized_function.error(), GetLine())));
			return false;
		}

		MidoriTraceable* worker_pointer = worker_function.GetPointer();
		if (worker_pointer == nullptr || !worker_pointer->IsTraceable<MidoriClosure>())
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, "SPAWN_WORKER expected a function value.", GetLine())));
			return false;
		}

		std::vector<SerializedValue> serialized_args;
		serialized_args.reserve(static_cast<size_t>(arg_count));
		for (int arg_index = 0; arg_index < arg_count; arg_index += 1)
		{
			MidoriValue argument = Pop();
			std::expected<SerializedValue, std::string> serialized_argument = ValueTransfer::Serialize(argument, *this, sharing);
			if (!serialized_argument.has_value())
			{
				m_instruction_pointer = ip;
				static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, serialized_argument.error(), GetLine())));
				return false;
			}
			serialized_args.emplace_back(std::move(serialized_argument.value()));
		}
		std::reverse(serialized_args.begin(), serialized_args.end());

		// The worker starts from a copy of this VM's globals, taken here on the
		// spawning thread because this heap is not safe to read from another one.
		// Globals are only ever defined once, so the copy is exactly what the
		// spawned function would see if it were called directly.
		std::vector<SerializedValue> serialized_globals;
		serialized_globals.reserve(m_global_vars->size());
		for (const MidoriValue& global_value : *m_global_vars)
		{
			std::expected<SerializedValue, std::string> serialized_global = ValueTransfer::Serialize(global_value, *this, sharing);
			if (!serialized_global.has_value())
			{
				m_instruction_pointer = ip;
				static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, serialized_global.error(), GetLine())));
				return false;
			}
			serialized_globals.emplace_back(std::move(serialized_global.value()));
		}

		const int worker_id = WorkerRegistry::GetInstance().SpawnWorker(m_owned_executable, std::move(serialized_function.value()), std::move(serialized_args), std::move(serialized_globals));
		Push(static_cast<MidoriInteger>(worker_id));
		return true;
	}
	case OpCode::JOIN_WORKER:
	{
		const int ok_tag = static_cast<int>(ReadByte(ip));
		const int err_tag = static_cast<int>(ReadByte(ip));
		const int cancelled_tag = static_cast<int>(ReadByte(ip));
		const int failed_tag = static_cast<int>(ReadByte(ip));

		// `join` evaluates to Result<T, WorkerError>. A worker that failed or was
		// cancelled becomes Err(...) for the joining code to handle; it no longer
		// terminates the joining VM.
		const int worker_id = static_cast<int>(Pop().GetInteger());
		std::expected<SerializedValue, WorkerError> worker_result = WorkerRegistry::GetInstance().JoinWorkerValue(worker_id);

		MidoriValue payload;
		int result_tag = ok_tag;
		if (worker_result.has_value())
		{
			std::expected<MidoriValue, std::string> deserialized_result = ValueTransfer::Deserialize(worker_result.value(), *this);
			if (!deserialized_result.has_value())
			{
				m_instruction_pointer = ip;
				static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, deserialized_result.error(), GetLine())));
				return false;
			}
			payload = deserialized_result.value();
		}
		else
		{
			MidoriTraceable* worker_error = AllocateTraceable(MidoriUnion());
			MidoriUnion& worker_error_ref = worker_error->GetTraceable<MidoriUnion>();
			if (worker_result.error().m_code == RuntimeErrorCode::WorkerCancelled)
			{
				worker_error_ref.m_index = cancelled_tag;
			}
			else
			{
				MidoriTuple failed_fields(1);
				failed_fields[0] = AllocateTraceable(MidoriText(worker_result.error().m_message.c_str()));
				worker_error_ref.m_values = std::move(failed_fields);
				worker_error_ref.m_index = failed_tag;
			}
			payload = worker_error;
			result_tag = err_tag;
		}

		MidoriTraceable* result = AllocateTraceable(MidoriUnion());
		MidoriUnion& result_ref = result->GetTraceable<MidoriUnion>();
		MidoriTuple result_fields(1);
		result_fields[0] = payload;
		result_ref.m_values = std::move(result_fields);
		result_ref.m_index = result_tag;
		Push(result);
		return true;
	}
	case OpCode::CHANNEL_CREATE:
	{
		const int capacity = static_cast<int>(Pop().GetInteger());
		const int channel_id = ChannelRegistry::GetInstance().CreateChannel(capacity);
		Push(static_cast<MidoriInteger>(channel_id));
		return true;
	}
	case OpCode::CHANNEL_SEND:
	{
		MidoriValue value = Pop();
		const int channel_id = static_cast<int>(Pop().GetInteger());
		std::expected<SerializedValue, std::string> serialized_value = ValueTransfer::Serialize(value, *this);
		if (!serialized_value.has_value())
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, serialized_value.error(), GetLine())));
			return false;
		}

		const ChannelOpStatus send_status = ChannelRegistry::GetInstance().Send(channel_id, std::move(serialized_value.value()), m_stop_token);
		if (send_status == ChannelOpStatus::Cancelled)
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine())));
			return false;
		}

		Push(send_status == ChannelOpStatus::Ok);
		return true;
	}
	case OpCode::CHANNEL_RECEIVE:
	{
		const int channel_id = static_cast<int>(Pop().GetInteger());
		ChannelReceiveResult received_value = ChannelRegistry::GetInstance().Receive(channel_id, m_stop_token);
		if (received_value.m_status == ChannelOpStatus::Cancelled)
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine())));
			return false;
		}
		if (received_value.m_status == ChannelOpStatus::Closed)
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, "Cannot receive from a closed and empty channel.", GetLine())));
			return false;
		}

		std::expected<MidoriValue, std::string> deserialized_value = ValueTransfer::Deserialize(received_value.m_value.value(), *this);
		if (!deserialized_value.has_value())
		{
			m_instruction_pointer = ip;
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, deserialized_value.error(), GetLine())));
			return false;
		}

		Push(deserialized_value.value());
		return true;
	}
	case OpCode::CHANNEL_CLOSE:
	{
		const int channel_id = static_cast<int>(Pop().GetInteger());
		ChannelRegistry::GetInstance().Close(channel_id);
		Push(MidoriValue());
		return true;
	}
	case OpCode::WORKER_IS_DONE:
	{
		const int worker_id = static_cast<int>(Pop().GetInteger());
		const bool is_done = WorkerRegistry::GetInstance().IsWorkerDone(worker_id);
		Push(is_done);
		return true;
	}
	case OpCode::WORKER_CANCEL:
	{
		const int worker_id = static_cast<int>(Pop().GetInteger());
		const bool cancelled = WorkerRegistry::GetInstance().CancelWorker(worker_id);
		Push(cancelled);
		return true;
	}
	default:
	{
		MIDORI_UNREACHABLE();
	}
	}
#endif
}

void VirtualMachine::InitializeProcEntryCache() noexcept
{
	if (!m_executable)
	{
		return;
	}

	int count = m_executable->GetProcedureCount();
	m_proc_entry_cache.resize(static_cast<size_t>(count));
	m_static_closure_cache.assign(static_cast<size_t>(count), nullptr);
	for (int i = 0; i < count; i += 1)
	{
		const BytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		m_proc_entry_cache[static_cast<size_t>(i)] = bytecode[0u];
	}
}


void VirtualMachine::InitializeStacks() noexcept
{
#ifdef _WIN32
	SYSTEM_INFO system_info;
	GetSystemInfo(&system_info);
	m_stack_page_size = static_cast<size_t>(system_info.dwPageSize);
#else
	const long page_size = sysconf(_SC_PAGESIZE);
	m_stack_page_size = page_size > 0 ? static_cast<size_t>(page_size) : 4096uz;
#endif

	const size_t value_stack_bytes = s_value_stack_size * sizeof(MidoriValue);
	const size_t value_usable_bytes = ((value_stack_bytes + m_stack_page_size - 1u) / m_stack_page_size) * m_stack_page_size;
	const size_t value_total_size = value_usable_bytes + m_stack_page_size;

#ifdef _WIN32
	m_value_stack_region = VirtualAlloc(nullptr, value_total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (m_value_stack_region != nullptr)
	{
		char* value_region = static_cast<char*>(m_value_stack_region);
		m_value_stack_begin = reinterpret_cast<MidoriValue*>(value_region + (value_usable_bytes - value_stack_bytes));
		m_value_stack_region_size = value_total_size;

		DWORD old_protect = 0;
		static_cast<void>(VirtualProtect(value_region + value_usable_bytes, m_stack_page_size, PAGE_NOACCESS, &old_protect));
	}
	else
	{
		m_value_stack_begin = static_cast<MidoriValue*>(std::malloc(value_stack_bytes));
	}
#else
	m_value_stack_region = mmap(nullptr, value_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m_value_stack_region != MAP_FAILED)
	{
		char* value_region = static_cast<char*>(m_value_stack_region);
		m_value_stack_begin = reinterpret_cast<MidoriValue*>(value_region + (value_usable_bytes - value_stack_bytes));
		m_value_stack_region_size = value_total_size;
		static_cast<void>(mprotect(value_region + value_usable_bytes, m_stack_page_size, PROT_NONE));
	}
	else
	{
		m_value_stack_region = nullptr;
		m_value_stack_begin = static_cast<MidoriValue*>(std::malloc(value_stack_bytes));
	}
#endif

	const size_t call_stack_bytes = s_call_stack_size * sizeof(CallFrame);
	const size_t call_usable_bytes = ((call_stack_bytes + m_stack_page_size - 1u) / m_stack_page_size) * m_stack_page_size;
	const size_t call_total_size = call_usable_bytes + m_stack_page_size;

#ifdef _WIN32
	m_call_stack_region = VirtualAlloc(nullptr, call_total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (m_call_stack_region != nullptr)
	{
		char* call_region = static_cast<char*>(m_call_stack_region);
		m_call_stack_begin = reinterpret_cast<CallFrame*>(call_region + (call_usable_bytes - call_stack_bytes));
		m_call_stack_region_size = call_total_size;

		DWORD old_protect = 0;
		static_cast<void>(VirtualProtect(call_region + call_usable_bytes, m_stack_page_size, PAGE_NOACCESS, &old_protect));
	}
	else
	{
		m_call_stack_begin = static_cast<CallFrame*>(std::malloc(call_stack_bytes));
	}
#else
	m_call_stack_region = mmap(nullptr, call_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m_call_stack_region != MAP_FAILED)
	{
		char* call_region = static_cast<char*>(m_call_stack_region);
		m_call_stack_begin = reinterpret_cast<CallFrame*>(call_region + (call_usable_bytes - call_stack_bytes));
		m_call_stack_region_size = call_total_size;
		static_cast<void>(mprotect(call_region + call_usable_bytes, m_stack_page_size, PROT_NONE));
	}
	else
	{
		m_call_stack_region = nullptr;
		m_call_stack_begin = static_cast<CallFrame*>(std::malloc(call_stack_bytes));
	}
#endif

	m_value_stack_base_pointer = m_value_stack_begin;
	m_value_stack_pointer = m_value_stack_base_pointer;
	m_call_stack_pointer = m_call_stack_begin;
}

VirtualMachine::~VirtualMachine()
{
	GarbageCollector::GarbageCollectionRoots roots;
	m_gc.ReclaimMemory(roots, m_allocator, true);

#ifdef _WIN32
	if (m_value_stack_region != nullptr)
	{
		VirtualFree(m_value_stack_region, 0, MEM_RELEASE);
	}
	else
	{
		std::free(m_value_stack_begin);
	}

	if (m_call_stack_region != nullptr)
	{
		VirtualFree(m_call_stack_region, 0, MEM_RELEASE);
	}
	else
	{
		std::free(m_call_stack_begin);
	}
#else
	if (m_value_stack_region != nullptr)
	{
		static_cast<void>(munmap(m_value_stack_region, m_value_stack_region_size));
	}
	else
	{
		std::free(m_value_stack_begin);
	}

	if (m_call_stack_region != nullptr)
	{
		static_cast<void>(munmap(m_call_stack_region, m_call_stack_region_size));
	}
	else
	{
		std::free(m_call_stack_begin);
	}
#endif
}

int VirtualMachine::TerminateExecution(RuntimeError error) noexcept
{
	m_last_error = std::move(error);
	return m_last_error->ExitCode();
}

int VirtualMachine::GetLine() noexcept
{
	for (int i : std::views::iota(0, m_executable->GetProcedureCount()))
	{
		const BytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		const OpCode* start = &*bytecode.cbegin();
		const OpCode* end = start + bytecode.GetByteCodeSize();

		if (m_instruction_pointer >= start && m_instruction_pointer < end)
		{
			return m_executable->GetLine(static_cast<int>(m_instruction_pointer - start), i);
		}
	}

	return 0;
}

RuntimeError VirtualMachine::GenerateRuntimeError(RuntimeErrorCode code, std::string_view message, int line) noexcept
{
	SourceLineCache source_line_cache(*m_executable);
	const int current_proc = GetProcedureIndexFromIP(m_instruction_pointer);
	const std::optional<RuntimeStackFrame> current_frame = ResolveStackTraceFrame(*m_executable, current_proc, line, source_line_cache);

	std::optional<CompilerErrorLocation> location = std::nullopt;
	if (current_frame.has_value())
	{
		location = current_frame->m_location;
	}

	return MidoriError::GenerateRuntimeError(code, message, std::move(location), GenerateStackTrace());
}

int VirtualMachine::GetProcedureIndexFromIP(InstructionPointer ip) noexcept
{
	for (int i : std::views::iota(0, m_executable->GetProcedureCount()))
	{
		const BytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		const OpCode* start = &*bytecode.cbegin();
		const OpCode* end = start + bytecode.GetByteCodeSize();

		if (ip >= start && ip < end)
		{
			return i;
		}
	}
	return -1;
}

int VirtualMachine::GetLineFromIP(InstructionPointer ip, int proc_index) noexcept
{
	if (proc_index < 0 || proc_index >= m_executable->GetProcedureCount())
	{
		return 0;
	}

	const BytecodeStream& bytecode = m_executable->GetBytecodeStream(proc_index);
	const OpCode* start = &*bytecode.cbegin();
	int offset = static_cast<int>(ip - start);

	return m_executable->GetLine(offset, proc_index);
}

std::vector<RuntimeStackFrame> VirtualMachine::GenerateStackTrace() noexcept
{
	SourceLineCache source_line_cache(*m_executable);
	std::vector<RuntimeStackFrame> frames;
	frames.reserve(static_cast<size_t>(m_call_stack_pointer - m_call_stack_begin) + 1u);

	const auto append_frame = [&frames](std::optional<RuntimeStackFrame> frame) -> void
	{
		if (!frame.has_value())
		{
			return;
		}

		if (!frames.empty() && CanCollapseRecursiveFrame(frames.back(), *frame))
		{
			frames.back().m_recursive_call_count += 1;
			return;
		}

		frames.emplace_back(std::move(*frame));
	};

	// Current frame (where error occurred)
	const int current_proc = GetProcedureIndexFromIP(m_instruction_pointer);
	const int current_line = GetLineFromIP(m_instruction_pointer, current_proc);
	append_frame(ResolveStackTraceFrame(*m_executable, current_proc, current_line, source_line_cache));

	// Walk the call stack
	for (CallStackPointer frame_ptr = m_call_stack_pointer; frame_ptr != m_call_stack_begin; )
	{
		--frame_ptr;
		const CallFrame& frame = *frame_ptr;
		const int proc_index = GetProcedureIndexFromIP(frame.m_return_ip);
		const int line = GetLineFromIP(frame.m_return_ip, proc_index);
		append_frame(ResolveStackTraceFrame(*m_executable, proc_index, line, source_line_cache));
	}

	if (frames.size() > static_cast<size_t>(s_max_stack_trace_depth))
	{
		frames.resize(static_cast<size_t>(s_max_stack_trace_depth));
	}

	return frames;
}

bool VirtualMachine::IsStackGuardFault(uintptr_t fault_address) const noexcept
{
	if (m_stack_page_size == 0u)
	{
		return false;
	}

	const uintptr_t value_guard_begin = reinterpret_cast<uintptr_t>(m_value_stack_begin) + (s_value_stack_size * sizeof(MidoriValue));
	const uintptr_t value_guard_end = value_guard_begin + m_stack_page_size;
	if (fault_address >= value_guard_begin && fault_address < value_guard_end)
	{
		return true;
	}

	const uintptr_t call_guard_begin = reinterpret_cast<uintptr_t>(m_call_stack_begin) + (s_call_stack_size * sizeof(CallFrame));
	const uintptr_t call_guard_end = call_guard_begin + m_stack_page_size;
	return fault_address >= call_guard_begin && fault_address < call_guard_end;
}

int VirtualMachine::CheckIndexBounds(MidoriValue index, MidoriInteger size) noexcept
{
	MidoriInteger val = index.GetInteger();
	if (val < 0ll || val >= size)
	{
		return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::IndexOutOfBounds, std::format("Index out of bounds at index: {}.", val), GetLine()));
	}
	else
	{
		return 0;
	}
}

int VirtualMachine::CheckNewArraySize(MidoriInteger size) noexcept
{
	if (size < 0)
	{
		return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::NegativeArraySize, "Array size cannot be negative.", GetLine()));
	}
	else if (size > MAX_ARRAY_SIZE)
	{
		return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::ArraySizeExceeded, "Array size exceeds maximum array size.", GetLine()));
	}
	return 0;
}

MidoriValue VirtualMachine::EnsureCellHandle(MidoriValue& slot, ValueStackPointer closure_slot) noexcept
{
	MidoriTraceable* ptr = slot.GetPointer();
	if (ptr != nullptr && m_gc.Contains(ptr) && ptr->IsTraceable<MidoriCellValue>())
	{
		return slot;
	}

	MidoriValue cell_value = AllocateTraceable(MidoriCellValue(slot));
	if (&slot != closure_slot)
	{
		slot = cell_value;
	}
	return cell_value;
}

void VirtualMachine::BuildGarbageCollectionRoots(GarbageCollector::GarbageCollectionRoots& roots) const noexcept
{
	roots.clear();

	size_t stack_count = 0uz;
	if (m_value_stack_begin != nullptr && m_value_stack_pointer != nullptr)
	{
		stack_count = static_cast<size_t>(m_value_stack_pointer - m_value_stack_begin);
	}

	size_t global_count = 0uz;
	if (m_global_vars != nullptr)
	{
		global_count = m_global_vars->size();
	}

	roots.reserve(stack_count + global_count + m_string_literal_cache.size() + m_small_string_pool.size() + 1uz);

	if (stack_count > 0uz)
	{
		for (MidoriValue* it = m_value_stack_begin; it != m_value_stack_pointer; ++it)
		{
			MidoriTraceable* ptr = it->GetPointer();
			if (ptr && m_gc.Contains(ptr))
			{
				roots.emplace_back(ptr);
			}
		}
	}

	if (m_global_vars != nullptr)
	{
		for (const MidoriValue& val : *m_global_vars)
		{
			MidoriTraceable* ptr = val.GetPointer();
			if (ptr != nullptr && m_gc.Contains(ptr))
			{
				roots.emplace_back(ptr);
			}
		}
	}

	if (m_curr_closure_traceable != nullptr)
	{
		roots.emplace_back(m_curr_closure_traceable);
	}

	for (MidoriTraceable* cached_string : m_string_literal_cache)
	{
		if (cached_string)
		{
			roots.emplace_back(cached_string);
		}
	}

	for (MidoriTraceable* cached_union : m_empty_union_cache)
	{
		if (cached_union)
		{
			roots.emplace_back(cached_union);
		}
	}

	for (const auto& [key, value] : m_small_string_pool)
	{
		if (value)
		{
			roots.emplace_back(value);
		}
	}
}

GarbageCollector::GarbageCollectionRoots VirtualMachine::GetGarbageCollectionRoots() const noexcept
{
	GarbageCollector::GarbageCollectionRoots roots;
	BuildGarbageCollectionRoots(roots);
	return roots;
}

MidoriTraceable* VirtualMachine::InternSmallString(const MidoriText& text) noexcept
{
	constexpr int SMALL_STRING_THRESHOLD = 4;

	int byte_length = text.GetByteLength();
	if (byte_length > SMALL_STRING_THRESHOLD)
	{
		return nullptr;
	}

	std::string_view key(text.GetCString(), static_cast<size_t>(byte_length));
	std::unordered_map<std::string_view, MidoriTraceable*>::iterator it = m_small_string_pool.find(key);
	if (it != m_small_string_pool.end())
	{
		return it->second;
	}

	MidoriText text_copy(text);
	MidoriTraceable* interned = AllocateTraceable(std::move(text_copy));
	m_small_string_pool[std::string_view(interned->GetTraceable<MidoriText>().GetCString(), static_cast<size_t>(byte_length))] = interned;
	return interned;
}

#ifndef MIDORI_PROFILE_OPCODES
#define MIDORI_PROFILE_OPCODES 0
#endif

#if MIDORI_PROFILE_OPCODES
namespace
{
	uint64_t s_opcode_pair_counts[256][256];
	size_t s_previous_opcode = 255u;

	void DumpOpcodeProfile()
	{
		std::FILE* file = std::fopen("marmot_opcode_profile.txt", "w");
		if (file == nullptr)
		{
			return;
		}
		for (size_t first = 0u; first < 256u; first += 1u)
		{
			for (size_t second = 0u; second < 256u; second += 1u)
			{
				if (s_opcode_pair_counts[first][second] != 0u)
				{
					std::fprintf(file, "%zu %zu %llu\n", first, second, static_cast<unsigned long long>(s_opcode_pair_counts[first][second]));
				}
			}
		}
		std::fclose(file);
	}
}
#endif

int VirtualMachine::ExecuteLoop() noexcept
{
	InstructionPointer ip = m_instruction_pointer;
	ValueStackPointer sp = m_value_stack_pointer;
	ValueStackPointer bp = m_value_stack_base_pointer;
	MidoriTuple* env = m_curr_environment;

	while (true)
	{

#if MIDORI_ENABLE_EXECUTION_TRACE
		if (MidoriBuild::ShouldEmitInternalDiagnostics())
		{
			Printer::Print("          ");
#ifdef __EMSCRIPTEN__
			std::for_each
			(
				m_value_stack_begin,
				bp - 1 < m_value_stack_begin ? m_value_stack_begin : bp - 1,
				[](MidoriValue value) -> void
				{
					Printer::Print<Printer::Color::YELLOW>(("[ "s + value.ToText().GetCString() + " ]"s));
				}
			);
			std::for_each
			(
				bp,
				sp,
				[](MidoriValue value) -> void
				{
					Printer::Print<Printer::Color::GREEN>(("[ "s + value.ToText().GetCString() + " ]"s));
				}
			);
#else
			std::for_each
			(
				std::execution::seq,
				m_value_stack_begin,
				bp - 1 < m_value_stack_begin ? m_value_stack_begin : bp - 1,
				[](MidoriValue value) -> void
				{
					Printer::Print<Printer::Color::YELLOW>(("[ "s + value.ToText().GetCString() + " ]"s));
				}
			);
			std::for_each
			(
				std::execution::seq,
				bp,
				sp,
				[](MidoriValue value) -> void
				{
					Printer::Print<Printer::Color::GREEN>(("[ "s + value.ToText().GetCString() + " ]"s));
				}
			);
#endif
			Printer::Print("\n");
			int dbg_instruction_pointer = -1;
			int dbg_proc_index = -1;

			for (int i : std::views::iota(0, m_executable->GetProcedureCount()))
			{
				const BytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
				const OpCode* start = &*bytecode.cbegin();
				const OpCode* end = start + bytecode.GetByteCodeSize();

				if (ip >= start && ip < end)
				{
					dbg_proc_index = i;
					dbg_instruction_pointer = static_cast<int>(ip - start);
				}
			}
#if MIDORI_ENABLE_DISASSEMBLY
			Disassembler::DisassembleInstruction(*m_executable, dbg_proc_index, dbg_instruction_pointer);
#endif
		}
#endif
		const InstructionPointer inst_ip = ip;
		OpCode instruction = ReadByte(ip);
#if MIDORI_PROFILE_OPCODES
		s_opcode_pair_counts[s_previous_opcode][static_cast<size_t>(instruction)] += 1u;
		s_previous_opcode = static_cast<size_t>(instruction);
#endif

		switch (instruction)
		{
		case OpCode::LOAD_STRING:
		{
			size_t index = static_cast<size_t>(ReadByte(ip));
			if (index >= m_string_literal_cache.size() || !m_string_literal_cache[index])
			{
				if (index >= m_string_literal_cache.size())
				{
					m_string_literal_cache.resize(index + 1, nullptr);
				}
				m_string_literal_cache[index] = AllocateTraceable(m_executable->GetStringPool()[index].data());
			}
			MidoriText& cached_text = m_string_literal_cache[index]->GetTraceable<MidoriText>();
			MidoriText text_copy(cached_text);
			MidoriTraceable* new_string = AllocateTraceable(std::move(text_copy));
			Push(sp, new_string);
			break;
		}
		case OpCode::LOAD_STRING_WIDE:
		{
			size_t index = static_cast<size_t>(ReadShort(ip));
			if (index >= m_string_literal_cache.size() || !m_string_literal_cache[index])
			{
				if (index >= m_string_literal_cache.size())
				{
					m_string_literal_cache.resize(index + 1, nullptr);
				}
				m_string_literal_cache[index] = AllocateTraceable(m_executable->GetStringPool()[index].data());
			}
			MidoriText& cached_text = m_string_literal_cache[index]->GetTraceable<MidoriText>();
			MidoriText text_copy(cached_text);
			MidoriTraceable* new_string = AllocateTraceable(std::move(text_copy));
			Push(sp, new_string);
			break;
		}
		case OpCode::INTEGER_CONSTANT:
		{
			Push(sp, ReadIntegerConstant(ip));
			break;
		}
		case OpCode::FLOAT_CONSTANT:
		{
			Push(sp, ReadFloatConstant(ip));
			break;
		}
		case OpCode::BYTE_CONSTANT:
		{
			Push(sp, ReadByteConstant(ip));
			break;
		}
		case OpCode::WORD_CONSTANT:
		{
			Push(sp, ReadWordConstant(ip));
			break;
		}
		case OpCode::OP_UNIT:
		{
			Push(sp, MidoriValue());
			break;
		}
		case OpCode::OP_TRUE:
		{
			Push(sp, true);
			break;
		}
		case OpCode::OP_FALSE:
		{
			Push(sp, false);
			break;
		}
		case OpCode::INT_MINUS_1:
		{
			Push(sp, -1LL);
			break;
		}
		case OpCode::INT_0:
		{
			Push(sp, 0LL);
			break;
		}
		case OpCode::INT_1:
		{
			Push(sp, 1LL);
			break;
		}
		case OpCode::INT_2:
		{
			Push(sp, 2LL);
			break;
		}
		case OpCode::INT_3:
		{
			Push(sp, 3LL);
			break;
		}
		case OpCode::INT_4:
		{
			Push(sp, 4LL);
			break;
		}
		case OpCode::INT_5:
		{
			Push(sp, 5LL);
			break;
		}
		case OpCode::INT_10:
		{
			Push(sp, 10LL);
			break;
		}
		case OpCode::CREATE_ARRAY:
		{
			int count = ReadThreeBytes(ip);
			MidoriArray arr(count);

			for (int i = count - 1; i >= 0; i -= 1)
			{
				arr[i] = Pop(sp);
			}

			Push(sp, AllocateTraceable(std::move(arr)));
			break;
		}
		case OpCode::CREATE_TUPLE:
		{
			int count = ReadThreeBytes(ip);
			MidoriTuple tuple(count);

			for (int i = count - 1; i >= 0; i -= 1)
			{
				tuple[i] = Pop(sp);
			}

			Push(sp, AllocateTraceable(std::move(tuple)));
			break;
		}
		case OpCode::GET_ARRAY:
		{
			MidoriValue& index = *(sp - 1);
			MidoriValue* arr_slot = sp - 2;
			MidoriValue arr = *arr_slot;
			MidoriArray& arr_ref = arr.GetPointer()->GetTraceable<MidoriArray>();
			m_instruction_pointer = inst_ip;

			int return_code = CheckIndexBounds(index, static_cast<MidoriInteger>(arr_ref.GetLength()));
			if (return_code != 0)
			{
				m_value_stack_pointer = arr_slot;
				m_value_stack_base_pointer = bp;
				m_curr_environment = env;
				return return_code;
			}

			*arr_slot = arr_ref[static_cast<int>(index.GetInteger())];
			sp = arr_slot + 1;

			break;
		}
		case OpCode::GET_TUPLE:
		{
			MidoriValue& index = *(sp - 1);
			MidoriValue* tuple_slot = sp - 2;
			MidoriValue tuple_value = *tuple_slot;
			MidoriTuple& tuple_ref = tuple_value.GetPointer()->GetTraceable<MidoriTuple>();
			m_instruction_pointer = inst_ip;

			int return_code = CheckIndexBounds(index, static_cast<MidoriInteger>(tuple_ref.GetLength()));
			if (return_code != 0)
			{
				m_value_stack_pointer = tuple_slot;
				m_value_stack_base_pointer = bp;
				m_curr_environment = env;
				return return_code;
			}

			*tuple_slot = tuple_ref[static_cast<int>(index.GetInteger())];
			sp = tuple_slot + 1;

			break;
		}
		case OpCode::UNPACK_TUPLE:
		{
			MidoriTuple& tuple = Pop(sp).GetPointer()->GetTraceable<MidoriTuple>();
			const int length = tuple.GetLength();
			for (int idx = 0; idx < length; idx += 1)
			{
				Push(sp, tuple[idx]);
			}
			break;
		}
		case OpCode::ADD_BACK_ARRAY:
		{
			MidoriValue val = Pop(sp);
			MidoriValue& arr = Peek(sp);

			m_gc.WriteBarrier(arr.GetPointer());
			MidoriArray& arr_ref = arr.GetPointer()->GetTraceable<MidoriArray>();
			arr_ref.AddBack(val);

			break;
		}
		case OpCode::GET_ARRAY_LENGTH:
		{
			MidoriValue arr = Pop(sp);
			MidoriArray& arr_ref = arr.GetPointer()->GetTraceable<MidoriArray>();
			MidoriInteger length = static_cast<MidoriInteger>(arr_ref.GetLength());
			Push(sp, length);
			break;
		}
		case OpCode::CREATE_INT_RANGE:
		{
			MidoriValue end = Pop(sp);
			MidoriValue step = Pop(sp);
			MidoriValue start = Pop(sp);

			MidoriIntRange range(start.GetInteger(), end.GetInteger(), step.GetInteger());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		case OpCode::CREATE_FLOAT_RANGE:
		{
			MidoriValue end = Pop(sp);
			MidoriValue step = Pop(sp);
			MidoriValue start = Pop(sp);

			MidoriFloatRange range(start.GetFloat(), end.GetFloat(), step.GetFloat());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		case OpCode::GET_RANGE_START:
		{
			MidoriValue range_ptr = Pop(sp);
			MidoriTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MidoriIntRange>())
			{
				Push(sp, ptr->GetTraceable<MidoriIntRange>().GetStart());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MidoriFloatRange>().GetStart());
			}
			break;
		}
		case OpCode::GET_RANGE_END:
		{
			MidoriValue range_ptr = Pop(sp);
			MidoriTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MidoriIntRange>())
			{
				Push(sp, ptr->GetTraceable<MidoriIntRange>().GetEnd());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MidoriFloatRange>().GetEnd());
			}
			break;
		}
		case OpCode::GET_RANGE_STEP:
		{
			MidoriValue range_ptr = Pop(sp);
			MidoriTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MidoriIntRange>())
			{
				Push(sp, ptr->GetTraceable<MidoriIntRange>().GetStep());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MidoriFloatRange>().GetStep());
			}
			break;
		}
		case OpCode::INT_TO_FLOAT:
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetInteger());
			break;
		}
		case OpCode::TEXT_TO_FLOAT:
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetPointer()->GetTraceable<MidoriText>().ToFloat());
			break;
		}
		case OpCode::FLOAT_TO_INT:
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetFloat());
			break;
		}
		case OpCode::TEXT_TO_INT:
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetPointer()->GetTraceable<MidoriText>().ToInteger());
			break;
		}
		case OpCode::FLOAT_TO_TEXT:
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromFloat(Peek(sp).GetFloat()));
			break;
		}
		case OpCode::INT_TO_TEXT:
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromInteger(Peek(sp).GetInteger()));
			break;
		}
		case OpCode::WORD_TO_TEXT:
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromWord(Peek(sp).GetWord()));
			break;
		}
		case OpCode::BYTE_TO_INT:
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetByte());
			break;
		}
		case OpCode::INT_TO_BYTE:
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetInteger() & 0xFF);
			break;
		}
		case OpCode::BYTE_TO_WORD:
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetByte());
			break;
		}
		case OpCode::WORD_TO_BYTE:
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetWord() & 0xFF);
			break;
		}
		case OpCode::WORD_TO_INT:
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetWord());
			break;
		}
		case OpCode::INT_TO_WORD:
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetInteger());
			break;
		}
		case OpCode::BYTE_TO_FLOAT:
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetByte());
			break;
		}
		case OpCode::FLOAT_TO_BYTE:
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetFloat());
			break;
		}
		case OpCode::WORD_TO_FLOAT:
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetWord());
			break;
		}
		case OpCode::FLOAT_TO_WORD:
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetFloat());
			break;
		}
		case OpCode::LEFT_SHIFT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() << right.GetInteger();
			break;
		}
		case OpCode::RIGHT_SHIFT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() >> right.GetInteger();

			break;
		}
		case OpCode::LEFT_SHIFT_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() << right.GetByte());

			break;
		}
		case OpCode::RIGHT_SHIFT_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() >> right.GetByte());

			break;
		}
		case OpCode::LEFT_SHIFT_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() << right.GetWord();

			break;
		}
		case OpCode::RIGHT_SHIFT_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() >> right.GetWord();

			break;
		}
		case OpCode::BITWISE_AND:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() & right.GetInteger();

			break;
		}
		case OpCode::BITWISE_OR:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() | right.GetInteger();

			break;
		}
		case OpCode::BITWISE_XOR:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() ^ right.GetInteger();

			break;
		}
		case OpCode::BITWISE_NOT:
		{
			MidoriValue& right = Peek(sp);

			right = ~right.GetInteger();

			break;
		}
		case OpCode::ADD_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() + right.GetFloat();

			break;
		}
		case OpCode::SUBTRACT_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() - right.GetFloat();

			break;
		}
		case OpCode::MULTIPLY_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() * right.GetFloat();

			break;
		}
		case OpCode::DIVIDE_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() / right.GetFloat();

			break;
		}
		case OpCode::MODULO_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = std::fmod(left.GetFloat(), right.GetFloat());

			break;
		}
		case OpCode::ADD_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() + right.GetInteger();

			break;
		}
		case OpCode::SUBTRACT_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() - right.GetInteger();

			break;
		}
		case OpCode::MULTIPLY_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() * right.GetInteger();

			break;
		}
		case OpCode::DIVIDE_INTEGER:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() / right.GetInteger();

			break;
		}
		case OpCode::MODULO_INTEGER:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() % right.GetInteger();

			break;
		}
		case OpCode::ADD_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() + right.GetByte());

			break;
		}
		case OpCode::SUBTRACT_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() - right.GetByte());

			break;
		}
		case OpCode::MULTIPLY_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() * right.GetByte());

			break;
		}
		case OpCode::DIVIDE_BYTE:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() / right.GetByte());

			break;
		}
		case OpCode::MODULO_BYTE:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() % right.GetByte());

			break;
		}
		case OpCode::ADD_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() + right.GetWord();

			break;
		}
		case OpCode::SUBTRACT_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() - right.GetWord();

			break;
		}
		case OpCode::MULTIPLY_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() * right.GetWord();

			break;
		}
		case OpCode::DIVIDE_WORD:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() / right.GetWord();

			break;
		}
		case OpCode::MODULO_WORD:
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() % right.GetWord();

			break;
		}
		case OpCode::CONCAT_ARRAY:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			MidoriArray& left_value_vector_ref = left.GetPointer()->GetTraceable<MidoriArray>();
			MidoriArray& right_value_vector_ref = right.GetPointer()->GetTraceable<MidoriArray>();
			MidoriArray result = MidoriArray::Concatenate(left_value_vector_ref, right_value_vector_ref);

			left = AllocateTraceable(std::move(result));
			TryCollect(ip, sp, bp, env);
			break;
		}
		case OpCode::CONCAT_TEXT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			MidoriText& left_value_string_ref = left.GetPointer()->GetTraceable<MidoriText>();
			MidoriText& right_value_string_ref = right.GetPointer()->GetTraceable<MidoriText>();

			MidoriText result = MidoriText::Concatenate(left_value_string_ref, right_value_string_ref);

			left = AllocateTraceable(std::move(result));
			TryCollect(ip, sp, bp, env);
			break;
		}
		case OpCode::EXTEND_ARRAY:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			m_gc.WriteBarrier(left.GetPointer());
			left.GetPointer()->GetTraceable<MidoriArray>().Extend(right.GetPointer()->GetTraceable<MidoriArray>());

			break;
		}
		case OpCode::EXTEND_TEXT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left.GetPointer()->GetTraceable<MidoriText>().Append(right.GetPointer()->GetTraceable<MidoriText>());

			break;
		}
		case OpCode::ADD_LOCAL_INT:
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));
			ip += 3;

			MidoriValue& slot = *(bp + local_index);
			MidoriInteger result = slot.GetInteger() + imm;
			slot = result;
			Push(sp, result);
			break;
		}
		case OpCode::PUSH_LOCAL_SUB_INT:
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));
			ip += 1;

			Push(sp, (bp + local_index)->GetInteger() - imm);
			break;
		}
		case OpCode::IF_LOCAL_LE_INT:
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));
			ip += 1;
			int offset = ReadShort(ip);

			if (!((bp + local_index)->GetInteger() <= imm))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_LOCAL_GE_LOCAL:
		{
			int left_index = static_cast<int>(ReadByte(ip));
			ip += 1;
			int right_index = static_cast<int>(ReadByte(ip));
			ip += 1;
			int offset = ReadShort(ip);

			if (!((bp + left_index)->GetInteger() >= (bp + right_index)->GetInteger()))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::GET_LOCAL2:
		{
			int first_index = static_cast<int>(ReadByte(ip));
			ip += 1;
			int second_index = static_cast<int>(ReadByte(ip));

			Push(sp, *(bp + first_index));
			Push(sp, *(bp + second_index));
			break;
		}
		case OpCode::ADD_ASSIGN_INT:
		{
			MidoriValue value = Pop(sp);
			MidoriValue& var = Peek(sp);
			var = var.GetInteger() + value.GetInteger();
			break;
		}
		case OpCode::SUB_ASSIGN_INT:
		{
			MidoriValue value = Pop(sp);
			MidoriValue& var = Peek(sp);
			var = var.GetInteger() - value.GetInteger();
			break;
		}
		case OpCode::EQUAL_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() == right.GetFloat();

			break;
		}
		case OpCode::NOT_EQUAL_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() != right.GetFloat();

			break;
		}
		case OpCode::GREATER_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() > right.GetFloat();

			break;
		}
		case OpCode::GREATER_EQUAL_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() >= right.GetFloat();

			break;
		}
		case OpCode::LESS_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() < right.GetFloat();

			break;
		}
		case OpCode::LESS_EQUAL_FLOAT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() <= right.GetFloat();

			break;
		}
		case OpCode::EQUAL_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() == right.GetInteger();

			break;
		}
		case OpCode::NOT_EQUAL_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() != right.GetInteger();

			break;
		}
		case OpCode::GREATER_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() > right.GetInteger();

			break;
		}
		case OpCode::GREATER_EQUAL_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() >= right.GetInteger();

			break;
		}
		case OpCode::LESS_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() < right.GetInteger();

			break;
		}
		case OpCode::LESS_EQUAL_INTEGER:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() <= right.GetInteger();

			break;
		}
		case OpCode::EQUAL_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() == right.GetByte();

			break;
		}
		case OpCode::NOT_EQUAL_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() != right.GetByte();

			break;
		}
		case OpCode::GREATER_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() > right.GetByte();

			break;
		}
		case OpCode::GREATER_EQUAL_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() >= right.GetByte();

			break;
		}
		case OpCode::LESS_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() < right.GetByte();

			break;
		}
		case OpCode::LESS_EQUAL_BYTE:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() <= right.GetByte();

			break;
		}
		case OpCode::EQUAL_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() == right.GetWord();

			break;
		}
		case OpCode::NOT_EQUAL_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() != right.GetWord();

			break;
		}
		case OpCode::GREATER_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() > right.GetWord();

			break;
		}
		case OpCode::GREATER_EQUAL_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() >= right.GetWord();

			break;
		}
		case OpCode::LESS_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() < right.GetWord();

			break;
		}
		case OpCode::LESS_EQUAL_WORD:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() <= right.GetWord();

			break;
		}
		case OpCode::EQUAL_TEXT:
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetPointer()->GetTraceable<MidoriText>() == right.GetPointer()->GetTraceable<MidoriText>();

			break;
		}
		case OpCode::NOT:
		{
			MidoriValue& value = Peek(sp);
			value = !value.GetBool();
			break;
		}
		case OpCode::NEGATE_FLOAT:
		{
			MidoriValue& value = Peek(sp);
			value = -value.GetFloat();
			break;
		}
		case OpCode::NEGATE_INTEGER:
		{
			MidoriValue& value = Peek(sp);
			value = -value.GetInteger();
			break;
		}
		case OpCode::JUMP_IF_FALSE:
		{
			MidoriValue value = Peek(sp);

			int offset = ReadShort(ip);
			if (!value.GetBool())
			{
				ip += offset;
			}
			break;
		}
		case OpCode::JUMP_IF_TRUE:
		{
			MidoriValue value = Peek(sp);

			int offset = ReadShort(ip);
			if (value.GetBool())
			{
				ip += offset;
			}
			break;
		}
		case OpCode::JUMP:
		{
			int offset = ReadShort(ip);
			ip += offset;
			break;
		}
		case OpCode::JUMP_BACK:
		{
			int offset = ReadShort(ip);
			ip -= offset;
			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}
			TryCollect(ip, sp, bp, env);
			break;
		}
		case OpCode::IF_INTEGER_LESS:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left < right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_INTEGER_LESS_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left <= right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_INTEGER_GREATER:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left > right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_INTEGER_GREATER_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left >= right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_INTEGER_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left == right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_INTEGER_NOT_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriInteger right = Pop(sp).GetInteger();
			MidoriInteger left = Pop(sp).GetInteger();

			if (!(left != right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_LESS:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left < right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_LESS_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left <= right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_GREATER:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left > right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_GREATER_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left >= right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left == right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::IF_FLOAT_NOT_EQUAL:
		{
			int offset = ReadShort(ip);
			MidoriFloat right = Pop(sp).GetFloat();
			MidoriFloat left = Pop(sp).GetFloat();

			if (!(left != right))
			{
				ip += offset;
			}
			break;
		}
		case OpCode::LOAD_TAG:
		{
			MidoriValue union_val = Pop(sp);
			MidoriUnion& union_ref = union_val.GetPointer()->GetTraceable<MidoriUnion>();

			for (int i = 0; i < union_ref.m_values.GetLength(); i += 1)
			{
				Push(sp, union_ref.m_values[i]);
			}

			Push(sp, static_cast<MidoriInteger>(union_ref.m_index));
			break;
		}
		case OpCode::GET_TAG:
		{
			MidoriValue union_val = Pop(sp);
			MidoriUnion& union_ref = union_val.GetPointer()->GetTraceable<MidoriUnion>();
			Push(sp, static_cast<MidoriInteger>(union_ref.m_index));
			break;
		}
		case OpCode::SET_TAG:
		{
			int tag = static_cast<int>(ReadByte(ip));
			MidoriUnion& union_ref = Peek(sp).GetPointer()->GetTraceable<MidoriUnion>();
			union_ref.m_index = tag;
			break;
		}
		case OpCode::CALL_FOREIGN:
		{
			MidoriValue foreign_function_name = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));
			uint8_t return_type = static_cast<uint8_t>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!foreign_function_name.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalFFITypeError, std::format("Type error: expected function name (Text), but got {}.", foreign_function_name.ToText().GetCString()), GetLine()));
			}
#endif

			MidoriText& foreign_function_name_ref = foreign_function_name.GetPointer()->GetTraceable<MidoriText>();

			FFIFunction proc = nullptr;
			std::optional<size_t> ffi_idx = MidoriFFIRegistry::FindIndex(foreign_function_name_ref.GetCString());
			if (ffi_idx.has_value())
			{
				if (m_is_worker && ffi_idx.value() == MidoriFFIRegistry::ExitBuiltinIndex())
				{
					SyncMachineState(ip, sp, bp, env);
					return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerExited, "Worker exited. Exiting the process from a worker would end the whole program, so the worker fails instead and the joiner receives Err(Failed(...)). A panic inside a worker takes this path.", GetLine()));
				}

				proc = m_ffi_table[ffi_idx.value()];
			}
			else
			{
				std::optional<FFIFunction> dynamic_func = m_dynamic_ffi_registry.FindFunction(foreign_function_name_ref.GetCString());
				if (dynamic_func.has_value())
				{
					proc = dynamic_func.value();
				}
			}

			if (proc == nullptr)
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::FFIFunctionNotFound, std::format("Failed to load foreign function '{}'.", foreign_function_name_ref.GetCString()), GetLine()));
			}

			m_ffi_array_args.clear();
			if (m_ffi_array_args.capacity() < static_cast<size_t>(arity))
			{
				m_ffi_array_args.reserve(static_cast<size_t>(arity));
			}
			for (int i = arity - 1; i >= 0; i -= 1)
			{
				size_t idx = static_cast<size_t>(i);
				MidoriValue arg = Pop(sp);

				if (m_gc.Contains(arg.GetPointer()))
				{
					MidoriTraceable* ptr = arg.GetPointer();
					if (ptr->IsTraceable<MidoriText>())
					{
						m_ffi_args[static_cast<size_t>(idx)] = (void*)ptr->GetTraceable<MidoriText>().GetCString();
					}
					else if (ptr->IsTraceable<MidoriArray>())
					{
						MidoriArray& array = ptr->GetTraceable<MidoriArray>();
						FFIArrayArgument array_arg;
						array_arg.data = &array[0u];
						array_arg.length = array.GetLength();
						m_ffi_array_args.push_back(array_arg);
						m_gc.WriteBarrier(ptr);
						m_ffi_args[static_cast<size_t>(idx)] = &m_ffi_array_args.back();
					}
					else
					{
						m_ffi_args[static_cast<size_t>(idx)] = nullptr;
					}
				}
				else
				{
					m_ffi_args[idx] = reinterpret_cast<void*>(static_cast<uintptr_t>(arg.GetRawBits()));
				}
			}

			MidoriValue return_val;
			proc(m_ffi_args.data(), reinterpret_cast<void*>(&return_val));

			if (return_type == 1)
			{
				int64_t ptr_val = return_val.GetInteger();
				if (ptr_val == 0)
				{
					Push(sp, AllocateTraceable(""));
				}
				else
				{
					char* ffi_string = reinterpret_cast<char*>(ptr_val);
					Push(sp, AllocateTraceable(ffi_string));
					std::free(ffi_string);
				}
			}
			else if (return_type == 2)
			{
				struct FFIArray
				{
					void* data;
					int length;
				};

				int64_t ptr_val = return_val.GetInteger();
				if (ptr_val == 0)
				{
					Push(sp, AllocateTraceable(MidoriArray()));
				}
				else
				{
					FFIArray* ffi_array = reinterpret_cast<FFIArray*>(ptr_val);
					MidoriValue* ffi_array_data = static_cast<MidoriValue*>(ffi_array->data);
					int length = ffi_array->length;

					MidoriArray wrapped_array = MidoriArray::FromFFI(ffi_array_data, length);
					Push(sp, AllocateTraceable(std::move(wrapped_array)));

					std::free(ffi_array);
				}
			}
			else
			{
				Push(sp, return_val);
			}

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		case OpCode::CALL_FOREIGN_INDEXED:
		{
			uint8_t ffi_index = static_cast<uint8_t>(ReadByte(ip));
			int arity = static_cast<int>(ReadByte(ip));
			uint8_t return_type = static_cast<uint8_t>(ReadByte(ip));

			if (m_is_worker && static_cast<size_t>(ffi_index) == MidoriFFIRegistry::ExitBuiltinIndex())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerExited, "Worker exited. Exiting the process from a worker would end the whole program, so the worker fails instead and the joiner receives Err(Failed(...)). A panic inside a worker takes this path.", GetLine()));
			}

			const FFIEntry& ffi_entry = MidoriFFIRegistry::GetEntry(ffi_index);
			FFIFunction proc = m_ffi_table[ffi_index];

			m_ffi_array_args.clear();
			if (m_ffi_array_args.capacity() < static_cast<size_t>(arity))
			{
				m_ffi_array_args.reserve(static_cast<size_t>(arity));
			}
			for (int i = arity - 1; i >= 0; i -= 1)
			{
				size_t idx = static_cast<size_t>(i);
				MidoriValue arg = Pop(sp);
				const FFIArgumentKind arg_kind = ffi_entry.m_arg_kinds[idx];
				MidoriTraceable* ptr = arg.GetPointer();
				const bool is_managed_traceable = ptr != nullptr && m_gc.Contains(ptr);

				switch (arg_kind)
				{
				case FFIArgumentKind::CString:
					if (is_managed_traceable && ptr->IsTraceable<MidoriText>())
					{
						m_ffi_args[idx] = (void*)ptr->GetTraceable<MidoriText>().GetCString();
					}
					else
					{
						m_ffi_args[idx] = nullptr;
					}
					break;
				case FFIArgumentKind::ArrayView:
					if (is_managed_traceable && ptr->IsTraceable<MidoriArray>())
					{
						MidoriArray& array = ptr->GetTraceable<MidoriArray>();
						FFIArrayArgument array_arg;
						array_arg.data = array.GetLength() > 0 ? static_cast<void*>(&array[0u]) : nullptr;
						array_arg.length = array.GetLength();
						m_ffi_array_args.push_back(array_arg);
						m_gc.WriteBarrier(ptr);
						m_ffi_args[idx] = &m_ffi_array_args.back();
					}
					else
					{
						m_ffi_args[idx] = nullptr;
					}
					break;
				case FFIArgumentKind::TraceableHandle:
					m_ffi_args[idx] = is_managed_traceable ? ptr : nullptr;
					break;
				case FFIArgumentKind::ValueHandle:
					m_ffi_value_args[idx] = arg;
					m_ffi_args[idx] = &m_ffi_value_args[idx];
					break;
				case FFIArgumentKind::RawValue:
				default:
					m_ffi_args[idx] = reinterpret_cast<void*>(static_cast<uintptr_t>(arg.GetRawBits()));
					break;
				}
			}

			MidoriValue return_val;
			proc(m_ffi_args.data(), reinterpret_cast<void*>(&return_val));

			FFIReturnKind return_kind = ffi_entry.m_return_kind;
			if (return_kind == FFIReturnKind::RawValue)
			{
				if (return_type == 1)
				{
					return_kind = FFIReturnKind::CString;
				}
				else if (return_type == 2)
				{
					return_kind = FFIReturnKind::ArrayValues;
				}
			}

			if (return_kind == FFIReturnKind::CString)
			{
				int64_t ptr_val = return_val.GetInteger();
				if (ptr_val == 0)
				{
					Push(sp, AllocateTraceable(""));
				}
				else
				{
					char* ffi_string = reinterpret_cast<char*>(ptr_val);
					Push(sp, AllocateTraceable(ffi_string));
					std::free(ffi_string);
				}
			}
			else if (return_kind == FFIReturnKind::ArrayValues)
			{
				struct FFIArray
				{
					void* data;
					int length;
				};

				int64_t ptr_val = return_val.GetInteger();
				if (ptr_val == 0)
				{
					Push(sp, AllocateTraceable(MidoriArray()));
				}
				else
				{
					FFIArray* ffi_array = reinterpret_cast<FFIArray*>(ptr_val);
					MidoriValue* ffi_array_data = static_cast<MidoriValue*>(ffi_array->data);
					int length = ffi_array->length;

					MidoriArray wrapped_array = MidoriArray::FromFFI(ffi_array_data, length);
					Push(sp, AllocateTraceable(std::move(wrapped_array)));

					std::free(ffi_array);
				}
			}
			else if (return_kind == FFIReturnKind::ArrayStrings)
			{
				struct FFIArray
				{
					void* data;
					int length;
				};

				int64_t ptr_val = return_val.GetInteger();
				if (ptr_val == 0)
				{
					Push(sp, AllocateTraceable(MidoriArray()));
				}
				else
				{
					FFIArray* ffi_array = reinterpret_cast<FFIArray*>(ptr_val);
					char** ffi_strings = static_cast<char**>(ffi_array->data);
					const int length = ffi_array->length;

					MidoriArray wrapped_array(length);
					for (int idx = 0; idx < length; idx += 1)
					{
						char* ffi_string = ffi_strings[idx];
						wrapped_array[idx] = AllocateTraceable(ffi_string != nullptr ? ffi_string : "");
						std::free(ffi_string);
					}

					std::free(ffi_strings);
					std::free(ffi_array);
					Push(sp, AllocateTraceable(std::move(wrapped_array)));
				}
			}
			else
			{
				Push(sp, return_val);
			}

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		case OpCode::CALL:
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().GetCString()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(bp, ip, env);

			MidoriClosure& closure = callable.GetPointer()->GetTraceable<MidoriClosure>();
			env = &closure.m_cell_values;

			ip = GetProcEntry(closure.m_proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::CALL_0:
		case OpCode::CALL_1:
		case OpCode::CALL_2:
		case OpCode::CALL_3:
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(instruction) - static_cast<int>(OpCode::CALL_0);

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().GetCString()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(bp, ip, env);

			MidoriClosure& closure = callable.GetPointer()->GetTraceable<MidoriClosure>();
			env = &closure.m_cell_values;

			ip = GetProcEntry(closure.m_proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::CALL_PROC:
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			int arity = static_cast<int>(ReadByte(ip));

			PushCallFrame(bp, ip, env);

			// Static functions have no captures, so no environment needed
			env = nullptr;
			ip = GetProcEntry(proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::CALL_PROC_0:
		case OpCode::CALL_PROC_1:
		case OpCode::CALL_PROC_2:
		case OpCode::CALL_PROC_3:
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			int arity = static_cast<int>(instruction) - static_cast<int>(OpCode::CALL_PROC_0);

			PushCallFrame(bp, ip, env);

			// Static functions have no captures, so no environment needed
			env = nullptr;
			ip = GetProcEntry(proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::CALL_GLOBAL:
		{
			int global_idx = ReadGlobalVariable(ip);
			int arity = static_cast<int>(ReadByte(ip));
			MidoriValue callable = (*m_global_vars)[global_idx];

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().GetCString()), GetLine()));
			}
#endif

			PushCallFrame(bp, ip, env);

			MidoriClosure& closure = callable.GetPointer()->GetTraceable<MidoriClosure>();
			env = &closure.m_cell_values;

			ip = GetProcEntry(closure.m_proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::CALL_GLOBAL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			int arity = static_cast<int>(ReadByte(ip));
			MidoriValue callable = (*m_global_vars)[global_idx];

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().GetCString()), GetLine()));
			}
#endif

			PushCallFrame(bp, ip, env);

			MidoriClosure& closure = callable.GetPointer()->GetTraceable<MidoriClosure>();
			env = &closure.m_cell_values;

			ip = GetProcEntry(closure.m_proc_index);
			bp = sp - arity;

			break;
		}
		case OpCode::TAIL_CALL:
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().GetCString()), GetLine()));
			}
#endif

			// Move arguments down to base pointer
			MidoriValue* args_source = sp - arity;
			if (arity > 0 && args_source != bp)
			{
				std::memmove(bp, args_source, arity * sizeof(MidoriValue));
			}
			sp = bp + arity;

			MidoriClosure& closure = callable.GetPointer()->GetTraceable<MidoriClosure>();
			env = &closure.m_cell_values;

			// Jump to the start of the function without creating a new call frame
			ip = GetProcEntry(closure.m_proc_index);

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		case OpCode::CONSTRUCT_STRUCT:
		{
			MidoriTraceable* new_struct = AllocateTraceable(MidoriStruct());
			int size = static_cast<int>(ReadByte(ip));
			MidoriTuple args(size);

			for (int i = size - 1; i >= 0; i -= 1)
			{
				args[i] = Pop(sp);
			}

			MidoriTuple& members = new_struct->GetTraceable<MidoriStruct>().m_values;
			members = std::move(args);

			Push(sp, new_struct);
			break;
		}
		case OpCode::CONSTRUCT_UNION:
		{
			MidoriTraceable* new_union = AllocateTraceable(MidoriUnion());

			int size = static_cast<int>(ReadByte(ip));
			MidoriTuple args(size);

			for (int i = size - 1; i >= 0; i -= 1)
			{
				args[i] = Pop(sp);
			}

			MidoriTuple& members = new_union->GetTraceable<MidoriUnion>().m_values;
			members = std::move(args);

			Push(sp, new_union);
			break;
		}
		case OpCode::LOAD_EMPTY_UNION:
		{
			const size_t tag = static_cast<size_t>(ReadByte(ip));
			MidoriTraceable*& cached = m_empty_union_cache[tag];
			if (cached == nullptr)
			{
				cached = AllocateTraceable(MidoriUnion());
				cached->GetTraceable<MidoriUnion>().m_index = static_cast<int>(tag);
			}

			Push(sp, cached);
			break;
		}
		case OpCode::MAKE_CLOSURE:
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			Push(sp, AllocateTraceable(MidoriClosure{ .m_cell_values = MidoriTuple(), .m_proc_index = proc_index }));
			break;
		}
		case OpCode::MAKE_FUNCTION:
		{
			int proc_index = static_cast<int>(ReadByte(ip));

			size_t cache_index = static_cast<size_t>(proc_index);
			if (cache_index < m_static_closure_cache.size() && m_static_closure_cache[cache_index])
			{
				Push(sp, m_static_closure_cache[cache_index]);
			}
			else
			{
				MidoriTraceable* closure = AllocateTraceable(MidoriClosure{.m_cell_values = MidoriTuple(), .m_proc_index = proc_index});
				if (cache_index < m_static_closure_cache.size())
				{
					m_static_closure_cache[cache_index] = closure;
				}
				Push(sp, closure);
			}

			break;
		}
		case OpCode::BIND_CAPTURES:
		{
			int total_count = static_cast<int>(ReadByte(ip));

			MidoriTuple& closure_env = (sp - 1)->GetPointer()->GetTraceable<MidoriClosure>().m_cell_values;
			int parent_count = env ? env->GetLength() : 0;
			int local_capture_count = (total_count > parent_count) ? (total_count - parent_count) : 0;

			MidoriTuple new_env(total_count);
			MidoriValue* closure_slot = sp - 1;

			// Copy parent environment
			if (env)
			{
				for (int i = 0; i < parent_count; i += 1)
				{
					new_env[i] = (*env)[i];
				}
			}

			// Capture local variables
			for (int i = 0; i < local_capture_count; i += 1)
			{
				MidoriValue& local_slot = *(bp + i);
				new_env[parent_count + i] = EnsureCellHandle(local_slot, closure_slot);
			}

			// Closure is freshly allocated by the preceding MAKE_CLOSURE and cannot have
			// tenured (no GC safepoint runs between allocation and here), so this barrier
			// is normally a no-op. Kept as insurance against future codegen/safepoint changes.
			m_gc.WriteBarrier(closure_slot->GetPointer());
			closure_env = std::move(new_env);
			break;
		}
		case OpCode::DEFINE_GLOBAL:
		{
			MidoriValue value = Pop(sp);
			int global_idx = ReadGlobalVariable(ip);
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = value;
			break;
		}
		case OpCode::GET_GLOBAL:
		{
			int global_idx = ReadGlobalVariable(ip);
			Push(sp, (*m_global_vars)[global_idx]);
			break;
		}
		case OpCode::SET_GLOBAL:
		{
			int global_idx = ReadGlobalVariable(ip);
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = Peek(sp);
			break;
		}
		case OpCode::GET_LOCAL:
		{
			int offset = static_cast<int>(ReadByte(ip));
			Push(sp, *(bp + offset));
			break;
		}
		case OpCode::SET_LOCAL:
		{
			int offset = static_cast<int>(ReadByte(ip));
			*(bp + offset) = Peek(sp);
			break;
		}
		case OpCode::GET_LOCAL_CELL:
		{
			int offset = static_cast<int>(ReadByte(ip));
			MidoriValue& slot = *(bp + offset);
			MidoriTraceable* ptr = slot.GetPointer();
			if (ptr != nullptr && m_gc.Contains(ptr) && ptr->IsTraceable<MidoriCellValue>())
			{
				Push(sp, ptr->GetTraceable<MidoriCellValue>().GetValue());
			}
			else
			{
				Push(sp, slot);
			}
			break;
		}
		case OpCode::SET_LOCAL_CELL:
		{
			int offset = static_cast<int>(ReadByte(ip));
			MidoriValue& slot = *(bp + offset);
			MidoriValue value = Peek(sp);
			MidoriTraceable* ptr = slot.GetPointer();
			if (ptr != nullptr && m_gc.Contains(ptr) && ptr->IsTraceable<MidoriCellValue>())
			{
				m_gc.WriteBarrier(ptr);
				ptr->GetTraceable<MidoriCellValue>().GetValue() = value;
			}
			else
			{
				slot = value;
			}
			break;
		}
		case OpCode::GET_CELL:
		{
			int offset = static_cast<int>(ReadByte(ip));
#if MIDORI_DEBUG_FULL
			if (!env)
			{
				SyncMachineState(ip, sp, bp, env);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, "GET_CELL called with null environment - function has captures but was called via CALL_PROC", GetLine()));
			}
#endif
			MidoriValue cell_value = (*env)[offset].GetPointer()->GetTraceable<MidoriCellValue>().GetValue();
			Push(sp, cell_value);
			break;
		}
		case OpCode::SET_CELL:
		{
			int offset = static_cast<int>(ReadByte(ip));
			MidoriTraceable* cell_owner = (*env)[offset].GetPointer();
			m_gc.WriteBarrier(cell_owner);
			cell_owner->GetTraceable<MidoriCellValue>().GetValue() = Peek(sp);
			break;
		}
		case OpCode::MAKE_CELL:
		{
			// The initial value stays on the stack, and so stays rooted, until the
			// cell holding it replaces it.
			MidoriValue& top = Peek(sp);
			top = AllocateTraceable(MidoriMutableCell(top));
			TryCollect(ip, sp, bp, env);
			break;
		}
		case OpCode::READ_CELL:
		{
			MidoriValue& top = Peek(sp);
			top = top.GetPointer()->GetTraceable<MidoriMutableCell>().m_value;
			break;
		}
		case OpCode::WRITE_CELL:
		{
			MidoriValue value = Pop(sp);
			MidoriTraceable* cell = Pop(sp).GetPointer();
			m_gc.WriteBarrier(cell);
			cell->GetTraceable<MidoriMutableCell>().m_value = value;
			Push(sp, value);
			break;
		}
		case OpCode::DEFINE_GLOBAL_WIDE:
		{
			MidoriValue value = Pop(sp);
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = value;
			break;
		}
		case OpCode::GET_GLOBAL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			Push(sp, (*m_global_vars)[global_idx]);
			break;
		}
		case OpCode::SET_GLOBAL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = Peek(sp);
			break;
		}
		case OpCode::GET_LOCAL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			Push(sp, *(bp + offset));
			break;
		}
		case OpCode::SET_LOCAL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			*(bp + offset) = Peek(sp);
			break;
		}
		case OpCode::GET_LOCAL_CELL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			MidoriValue& slot = *(bp + offset);
			MidoriTraceable* ptr = slot.GetPointer();
			if (ptr != nullptr && m_gc.Contains(ptr) && ptr->IsTraceable<MidoriCellValue>())
			{
				Push(sp, ptr->GetTraceable<MidoriCellValue>().GetValue());
			}
			else
			{
				Push(sp, slot);
			}
			break;
		}
		case OpCode::SET_LOCAL_CELL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			MidoriValue& slot = *(bp + offset);
			MidoriValue value = Peek(sp);
			MidoriTraceable* ptr = slot.GetPointer();
			if (ptr != nullptr && m_gc.Contains(ptr) && ptr->IsTraceable<MidoriCellValue>())
			{
				m_gc.WriteBarrier(ptr);
				ptr->GetTraceable<MidoriCellValue>().GetValue() = value;
			}
			else
			{
				slot = value;
			}
			break;
		}
		case OpCode::GET_CELL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			MidoriValue cell_value = (*env)[offset].GetPointer()->GetTraceable<MidoriCellValue>().GetValue();
			Push(sp, cell_value);
			break;
		}
		case OpCode::SET_CELL_WIDE:
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			MidoriTraceable* cell_owner = (*env)[offset].GetPointer();
			m_gc.WriteBarrier(cell_owner);
			cell_owner->GetTraceable<MidoriCellValue>().GetValue() = Peek(sp);
			break;
		}
		case OpCode::GET_MEMBER:
		{
			int index = static_cast<int>(ReadByte(ip));
			MidoriValue value = Pop(sp);
			Push(sp, value.GetPointer()->GetTraceable<MidoriStruct>().m_values[index]);
			break;
		}
		case OpCode::POP:
		{
			--sp;
			break;
		}
		case OpCode::DUP:
		{
			Push(sp, Peek(sp));
			break;
		}
		case OpCode::SWAP:
		{
			MidoriValue first = Pop(sp);
			MidoriValue second = Pop(sp);
			Push(sp, first);
			Push(sp, second);
			break;
		}
		case OpCode::POP_LOCAL_SCOPE:
		{
			sp -= static_cast<int>(ReadByte(ip));
			break;
		}
		case OpCode::POP_VALUES:
		{
			sp -= static_cast<int>(ReadByte(ip));
			break;
		}
		case OpCode::POP_BLOCK_SCOPE:
		{
			MidoriValue final_value = Pop(sp);
			sp -= static_cast<int>(ReadByte(ip));
			Push(sp, final_value);
			break;
		}
		case OpCode::POP_MATCH_SCOPE:
		{
			MidoriValue final_value = Pop(sp);
			sp -= static_cast<int>(ReadByte(ip));
			Push(sp, final_value);
			break;
		}
		case OpCode::RETURN:
		{
			MidoriValue value = Pop(sp);
			--m_call_stack_pointer;
			const CallFrame& frame = *m_call_stack_pointer;

			// Callee's bp points to where args started, which is our return point
			ValueStackPointer return_point = bp;

			bp = frame.m_return_bp;
			sp = return_point;
			ip = frame.m_return_ip;
			env = frame.m_closure_ptr;

			Push(sp, value);

			break;
		}
		case OpCode::HALT:
		{
#if MIDORI_PROFILE_OPCODES
			DumpOpcodeProfile();
#endif
			SyncMachineState(ip, sp, bp, env);
			return 0;
		}
		case OpCode::PUSH_PLACEHOLDER:
		{
			Push(sp, MidoriValue());
			break;
		}
		case OpCode::SPAWN_WORKER:
		case OpCode::JOIN_WORKER:
		case OpCode::CHANNEL_CREATE:
		case OpCode::CHANNEL_SEND:
		case OpCode::CHANNEL_RECEIVE:
		case OpCode::CHANNEL_CLOSE:
		case OpCode::WORKER_IS_DONE:
		case OpCode::WORKER_CANCEL:
		{
			SyncMachineState(ip, sp, bp, env);
			if (!ExecuteConcurrencyInstruction(instruction, ip))
			{
				return EXIT_FAILURE;
			}
			sp = m_value_stack_pointer;
			break;
		}
		default:
		{
			MIDORI_UNREACHABLE();
		}
		}
	}

}

#ifdef _WIN32
struct ExceptionInfo
{
	ULONG exception_code;
	ULONG_PTR exception_address;
	ULONG_PTR fault_address;
	bool captured;
};

static int CaptureExceptionFilter(EXCEPTION_POINTERS* ex_info, ExceptionInfo* out_info)
{
	const DWORD exception_code = ex_info->ExceptionRecord->ExceptionCode;
	if (exception_code == EXCEPTION_ACCESS_VIOLATION || exception_code == EXCEPTION_INT_DIVIDE_BY_ZERO)
	{
		out_info->exception_code = exception_code;
		out_info->exception_address = (ULONG_PTR)ex_info->ExceptionRecord->ExceptionAddress;
		out_info->fault_address =
			exception_code == EXCEPTION_ACCESS_VIOLATION
				? ex_info->ExceptionRecord->ExceptionInformation[1]
				: 0u;
		out_info->captured = true;
		return EXCEPTION_EXECUTE_HANDLER;
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

int VirtualMachine::ExecuteLoopWithStructuredExceptionHandling(uintptr_t& exception_code, uintptr_t& exception_address, uintptr_t& fault_address, bool& captured) noexcept
{
	ExceptionInfo ex_info = { 0, 0, 0, false };
	int execute_result = EXIT_FAILURE;
	bool completed = false;

	__try
	{
		execute_result = ExecuteLoop();
		completed = true;
	}
	__except (CaptureExceptionFilter(GetExceptionInformation(), &ex_info))
	{
	}

	exception_code = static_cast<uintptr_t>(ex_info.exception_code);
	exception_address = static_cast<uintptr_t>(ex_info.exception_address);
	fault_address = static_cast<uintptr_t>(ex_info.fault_address);
	captured = ex_info.captured;

	if (completed)
	{
		return execute_result;
	}

	return EXIT_FAILURE;
}
#endif

VirtualMachine::ExecuteResult VirtualMachine::Execute() noexcept
{
	m_last_error.reset();

	if (!m_ffi_table_initialized)
	{
		// Initialize FFI table with statically linked functions once per VM.
		const std::array<FFIEntry, MidoriFFIRegistry::BUILTIN_COUNT>& registry = MidoriFFIRegistry::GetTable();
		for (size_t i = 0u; i < MidoriFFIRegistry::BUILTIN_COUNT; i += 1u)
		{
			m_ffi_table[i] = registry[i].m_function;
		}
		m_ffi_table_initialized = true;
	}

#ifdef _WIN32
	uintptr_t exception_code = 0u;
	uintptr_t exception_address = 0u;
	uintptr_t fault_address = 0u;
	bool captured_exception = false;
	const int execute_result = ExecuteLoopWithStructuredExceptionHandling(exception_code, exception_address, fault_address, captured_exception);
	if (!captured_exception)
	{
		if (m_last_error.has_value())
		{
			return std::unexpected(std::move(*m_last_error));
		}
		return execute_result;
	}

	if (exception_code == EXCEPTION_INT_DIVIDE_BY_ZERO)
	{
		static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::DivisionByZero, "Division by zero.", GetLine())));
		return std::unexpected(std::move(*m_last_error));
	}

	if (IsStackGuardFault(fault_address))
	{
		static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::StackOverflow, "Stack overflow - exceeded maximum call depth.", GetLine())));
		return std::unexpected(std::move(*m_last_error));
	}

	char message[256];
	std::snprintf(
		message,
		sizeof(message),
		"Memory access violation - possible bytecode corruption or invalid operation (exception at %p, fault address %p).",
		reinterpret_cast<void*>(exception_address),
		reinterpret_cast<void*>(fault_address));
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::MemoryAccessViolation, message, GetLine())));
	return std::unexpected(std::move(*m_last_error));
#else
	UnixSignalInfo signal_info;
	sigjmp_buf jump_buffer;
	UnixSignalHandlerState handler_state;
	handler_state.m_jump_buffer = &jump_buffer;
	handler_state.m_signal_info = &signal_info;

	if (!InstallVirtualMachineSignalHandlers(handler_state))
	{
		const int result = ExecuteLoop();
		if (m_last_error.has_value())
		{
			return std::unexpected(std::move(*m_last_error));
		}
		return result;
	}

	s_active_unix_signal_handler = &handler_state;
	const int signal_result = sigsetjmp(jump_buffer, 1);
	if (signal_result == 0)
	{
		const int result = ExecuteLoop();
		s_active_unix_signal_handler = nullptr;
		RestoreVirtualMachineSignalHandlers(handler_state);
		if (m_last_error.has_value())
		{
			return std::unexpected(std::move(*m_last_error));
		}
		return result;
	}

	s_active_unix_signal_handler = nullptr;
	RestoreVirtualMachineSignalHandlers(handler_state);

	if (IsStackGuardFault(signal_info.m_fault_address))
	{
		static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::StackOverflow, "Stack overflow - exceeded maximum call depth.", GetLine())));
		return std::unexpected(std::move(*m_last_error));
	}

	if (signal_info.m_signal_number == SIGFPE)
	{
		static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::DivisionByZero, "Division by zero.", GetLine())));
		return std::unexpected(std::move(*m_last_error));
	}

	static_cast<void>(TerminateExecution
	(
		GenerateRuntimeError
		(
			RuntimeErrorCode::MemoryAccessViolation,
			std::format(
				"Memory access violation - possible bytecode corruption or invalid operation (signal {}, fault address 0x{:X}).",
				signal_info.m_signal_number,
				signal_info.m_fault_address),
			GetLine())
	));
	return std::unexpected(std::move(*m_last_error));
#endif
}




