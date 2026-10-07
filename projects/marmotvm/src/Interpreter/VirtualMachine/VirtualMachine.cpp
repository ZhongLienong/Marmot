#include "Bytecode/Format/Format.h"
#include "Support/Attributes/Attributes.h"
#include "Support/Diagnostics/Diagnostics.h"
#include "Support/Terminal/Terminal.h"
#include "Bytecode/Scalar/IntegerArithmetic.h"
#include "Interpreter/Channel/Channel.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"
#include "Interpreter/Worker/Worker.h"
#include "Bytecode/Disassembler/Disassembler.h"
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

// Each handler ends in its own indirect jump to the next one, so the branch
// predictor sees one jump per opcode instead of a single shared one. MSVC has
// no labels-as-values, and Wasm lowers them back to a switch.
//
// The compiler gives each handler that jump by copying the shared dispatch
// block into it, and never copies a block that is its own successor. A handler
// that compiles to nothing (WORD_TO_INT reinterprets the same bits) would
// merge into the dispatch block and make it one, and every handler would then
// share one jump again. The empty asm keeps each handler a block of its own.
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__EMSCRIPTEN__)
#define MIDORI_THREADED_DISPATCH 1
#define MIDORI_HANDLER(name) case VmOpCode::name: handler_##name: __asm__ volatile("");
#else
#define MIDORI_THREADED_DISPATCH 0
#define MIDORI_HANDLER(name) case VmOpCode::name:
#endif

namespace
{
	constexpr std::string_view ANONYMOUS_FUNCTION = "<anonymous>";

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
		explicit SourceLineCache(const VmExecutable& executable)
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
		const VmExecutable& m_executable;
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
		const size_t separator_index = raw_view.rfind(VmModuleSeparator);

		std::string_view display_name = raw_view;
		std::string_view module_name;
		if (separator_index != std::string_view::npos)
		{
			display_name = raw_view.substr(0u, separator_index);
			module_name = raw_view.substr(separator_index + 1u);
		}

		const bool is_module_bootstrap = display_name.starts_with(VM_MODULE_BOOTSTRAP_PREFIX);
		if (display_name.starts_with(VM_MAIN_PROCEDURE_PREFIX))
		{
			display_name = "main";
		}
		else if (display_name.empty())
		{
			display_name = ANONYMOUS_FUNCTION;
		}

		return ProcedureDisplayName{ std::string(display_name), std::string(module_name), is_module_bootstrap };
	}

	std::optional<RuntimeStackFrame> ResolveStackTraceFrame(const VmExecutable& executable, int proc_index, int line, SourceLineCache& source_line_cache)
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

VirtualMachine::VirtualMachine(VmExecutable&& executable) noexcept
	: m_owned_executable(std::make_shared<VmExecutable>(std::move(executable)))
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
	static constexpr VmOpCode s_halt_bytecode[] = { VmOpCode::HALT };
}

VirtualMachine::VirtualMachine(std::shared_ptr<const VmExecutable> shared_executable, int proc_index, const GlobalVariables* source_globals) noexcept
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

	PushCallFrame(m_value_stack_begin, &s_halt_bytecode[0], nullptr, nullptr);
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

	PushCallFrame(m_value_stack_begin, &s_halt_bytecode[0], m_curr_environment, m_curr_closure_traceable);
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

MIDORI_NOINLINE bool VirtualMachine::ExecuteConcurrencyInstruction(VmOpCode instruction, InstructionPointer& ip) noexcept
{
#ifdef MIDORI_WASM
	m_instruction_pointer = ip;
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::UnsupportedPlatformOperation, "Concurrency is not supported in the WebAssembly build.", GetLine())));
	return false;
#else
	switch (instruction)
	{
	case VmOpCode::SPAWN_WORKER:
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
	case VmOpCode::JOIN_WORKER:
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
	case VmOpCode::CHANNEL_CREATE:
	{
		const int capacity = static_cast<int>(Pop().GetInteger());
		const int channel_id = ChannelRegistry::GetInstance().CreateChannel(capacity);
		Push(static_cast<MidoriInteger>(channel_id));
		return true;
	}
	case VmOpCode::CHANNEL_SEND:
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
	case VmOpCode::CHANNEL_RECEIVE:
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
	case VmOpCode::CHANNEL_CLOSE:
	{
		const int channel_id = static_cast<int>(Pop().GetInteger());
		ChannelRegistry::GetInstance().Close(channel_id);
		Push(MidoriValue());
		return true;
	}
	case VmOpCode::WORKER_IS_DONE:
	{
		const int worker_id = static_cast<int>(Pop().GetInteger());
		const bool is_done = WorkerRegistry::GetInstance().IsWorkerDone(worker_id);
		Push(is_done);
		return true;
	}
	case VmOpCode::WORKER_CANCEL:
	{
		const int worker_id = static_cast<int>(Pop().GetInteger());
		const bool cancelled = WorkerRegistry::GetInstance().CancelWorker(worker_id);
		Push(cancelled);
		return true;
	}
	default:
	{
		std::unreachable();
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
		const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
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
	// Only reserved: committing the whole region would charge every VM, workers
	// included, its full size up front. The page after the region stays reserved
	// and PAGE_NOACCESS as the guard.
	m_value_stack_region = VirtualAlloc(nullptr, value_total_size, MEM_RESERVE, PAGE_NOACCESS);
	if (m_value_stack_region != nullptr)
	{
		char* value_region = static_cast<char*>(m_value_stack_region);
		m_value_stack_begin = reinterpret_cast<MidoriValue*>(value_region + (value_usable_bytes - value_stack_bytes));
		m_value_stack_region_size = value_total_size;
		static_cast<void>(CommitStackPages(reinterpret_cast<uintptr_t>(m_value_stack_begin)));
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
	m_call_stack_region = VirtualAlloc(nullptr, call_total_size, MEM_RESERVE, PAGE_NOACCESS);
	if (m_call_stack_region != nullptr)
	{
		char* call_region = static_cast<char*>(m_call_stack_region);
		m_call_stack_begin = reinterpret_cast<CallFrame*>(call_region + (call_usable_bytes - call_stack_bytes));
		m_call_stack_region_size = call_total_size;
		static_cast<void>(CommitStackPages(reinterpret_cast<uintptr_t>(m_call_stack_begin)));
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
		const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		const VmOpCode* start = &*bytecode.cbegin();
		const VmOpCode* end = start + bytecode.GetByteCodeSize();

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

	std::optional<RuntimeErrorLocation> location = std::nullopt;
	if (current_frame.has_value())
	{
		location = current_frame->m_location;
	}

	return RuntimeError(code, message, std::move(location), GenerateStackTrace());
}

int VirtualMachine::GetProcedureIndexFromIP(InstructionPointer ip) noexcept
{
	for (int i : std::views::iota(0, m_executable->GetProcedureCount()))
	{
		const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		const VmOpCode* start = &*bytecode.cbegin();
		const VmOpCode* end = start + bytecode.GetByteCodeSize();

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

	const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(proc_index);
	const VmOpCode* start = &*bytecode.cbegin();
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

// The dispatch loop keeps its instruction pointer in a local and stores it only
// where a runtime error can be raised, so after a guard-page fault
// m_instruction_pointer is wherever it was last stored, possibly in a function
// that has since returned. The call stack is written through the member, so its
// innermost frame is the call that overflowed.
void VirtualMachine::LocateStackOverflowAtInnermostCall() noexcept
{
	if (m_call_stack_pointer == m_call_stack_begin)
	{
		return;
	}

	--m_call_stack_pointer;
	m_instruction_pointer = m_call_stack_pointer->m_return_ip;
}

int VirtualMachine::IndexOutOfBounds(MidoriInteger index) noexcept
{
	return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::IndexOutOfBounds, std::format("Index out of bounds at index: {}.", index), GetLine()));
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

	roots.reserve(stack_count + global_count + static_cast<size_t>(m_call_stack_pointer - m_call_stack_begin) + m_string_literal_cache.size() + m_small_string_pool.size() + m_static_closure_cache.size() + 1uz);

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

	if (m_curr_closure_traceable != nullptr && m_gc.Contains(m_curr_closure_traceable))
	{
		roots.emplace_back(m_curr_closure_traceable);
	}

	for (CallStackPointer frame = m_call_stack_begin; frame != m_call_stack_pointer; ++frame)
	{
		if (frame->m_closure != nullptr && m_gc.Contains(frame->m_closure))
		{
			roots.emplace_back(frame->m_closure);
		}
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

	// A cached function is handed out again by MAKE_FUNCTION_WIDE, so it must
	// outlive every value that held it.
	for (MidoriTraceable* cached_function : m_static_closure_cache)
	{
		if (cached_function)
		{
			roots.emplace_back(cached_function);
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

int VirtualMachine::ExecuteLoop() noexcept
{
	InstructionPointer ip = m_instruction_pointer;
	ValueStackPointer sp = m_value_stack_pointer;
	ValueStackPointer bp = m_value_stack_base_pointer;
	MidoriTuple* env = m_curr_environment;
	MidoriTraceable* closure = m_curr_closure_traceable;
	CallStackPointer csp = m_call_stack_pointer;
	const InstructionPointer* const proc_entries = m_proc_entry_cache.data();

#if MIDORI_ENABLE_OPCODE_METRICS
	const bool opcode_metrics = RuntimeDiagnostics::OpcodeMetricsEnabled();
	std::array<uint64_t, 256> opcode_counts{};
	struct OpcodeReport
	{
		bool m_enabled;
		const std::array<uint64_t, 256>& m_counts;
		~OpcodeReport()
		{
			if (m_enabled)
			{
				std::print(RuntimeDiagnostics::Output(), "Opcode metrics (this VM):\n");
				for (size_t index = 0uz; index < m_counts.size(); index += 1uz)
				{
					if (m_counts[index] != 0u)
					{
						std::print(RuntimeDiagnostics::Output(), "  {}: {}\n", VmOpCodeTable::Name(static_cast<VmOpCode>(index)), m_counts[index]);
					}
				}
			}
		}
	};
	const OpcodeReport opcode_report{opcode_metrics, opcode_counts};
#endif

#if MIDORI_THREADED_DISPATCH
	static const std::array<void*, VmOpCodeTable::COUNT> s_dispatch_table =
	{
#define MARMOT_OPCODE(name, length) &&handler_##name,
#include "Bytecode/Executable/OpCodes.def"
#undef MARMOT_OPCODE
	};
#endif

	while (true)
	{

#if MIDORI_ENABLE_EXECUTION_TRACE
		if (RuntimeDiagnostics::TraceEnabled())
		{
			std::print(RuntimeDiagnostics::Output(), "Stack:");
			for (ValueStackPointer value = m_value_stack_begin; value < sp; value += 1)
			{
				std::print(RuntimeDiagnostics::Output(), " [ {} ]", value->ToText().View());
			}
			std::print(RuntimeDiagnostics::Output(), "\n");
			for (int index = 0; index < m_executable->GetProcedureCount(); index += 1)
			{
				const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(index);
				const VmOpCode* begin = &*bytecode.cbegin();
				if (ip >= begin && ip < begin + bytecode.GetByteCodeSize())
				{
					int offset = static_cast<int>(ip - begin);
					Disassembler::DisassembleInstruction(RuntimeDiagnostics::Output(), *m_executable, index, offset);
					break;
				}
			}
		}
#endif
		const InstructionPointer inst_ip = ip;
		VmOpCode instruction = ReadByte(ip);
#if MIDORI_ENABLE_OPCODE_METRICS
		if (opcode_metrics)
		{
			opcode_counts[static_cast<size_t>(instruction)] += 1u;
		}
#endif

#if MIDORI_THREADED_DISPATCH
		goto *s_dispatch_table[static_cast<size_t>(instruction)];
#endif
		switch (instruction)
		{
		MIDORI_HANDLER(LOAD_STRING_WIDE)
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
			// Shared by every load: lowering never extends a text literal in place.
			Push(sp, m_string_literal_cache[index]);
			break;
		}
		MIDORI_HANDLER(INTEGER_CONSTANT)
		{
			Push(sp, ReadIntegerConstant(ip));
			break;
		}
		MIDORI_HANDLER(FLOAT_CONSTANT)
		{
			Push(sp, ReadFloatConstant(ip));
			break;
		}
		MIDORI_HANDLER(BYTE_CONSTANT)
		{
			Push(sp, ReadByteConstant(ip));
			break;
		}
		MIDORI_HANDLER(WORD_CONSTANT)
		{
			Push(sp, ReadWordConstant(ip));
			break;
		}
		MIDORI_HANDLER(OP_UNIT)
		{
			Push(sp, MidoriValue());
			break;
		}
		MIDORI_HANDLER(OP_TRUE)
		{
			Push(sp, true);
			break;
		}
		MIDORI_HANDLER(OP_FALSE)
		{
			Push(sp, false);
			break;
		}
		MIDORI_HANDLER(INT_MINUS_1)
		{
			Push(sp, MidoriInteger{-1});
			break;
		}
		MIDORI_HANDLER(INT_0)
		{
			Push(sp, MidoriInteger{0});
			break;
		}
		MIDORI_HANDLER(INT_1)
		{
			Push(sp, MidoriInteger{1});
			break;
		}
		MIDORI_HANDLER(INT_2)
		{
			Push(sp, MidoriInteger{2});
			break;
		}
		MIDORI_HANDLER(INT_3)
		{
			Push(sp, MidoriInteger{3});
			break;
		}
		MIDORI_HANDLER(INT_4)
		{
			Push(sp, MidoriInteger{4});
			break;
		}
		MIDORI_HANDLER(INT_5)
		{
			Push(sp, MidoriInteger{5});
			break;
		}
		MIDORI_HANDLER(INT_10)
		{
			Push(sp, MidoriInteger{10});
			break;
		}
		MIDORI_HANDLER(CREATE_ARRAY)
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
		MIDORI_HANDLER(CREATE_TUPLE)
		{
			int count = ReadThreeBytes(ip);
			sp -= count;
			Push(sp, AllocateTraceable(std::in_place_type<MidoriTuple>, std::span<const MidoriValue>(sp, static_cast<size_t>(count))));
			break;
		}
		MIDORI_HANDLER(GET_ARRAY)
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
		MIDORI_HANDLER(GET_TUPLE)
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
		MIDORI_HANDLER(ADD_BACK_ARRAY)
		{
			MidoriValue val = Pop(sp);
			MidoriValue& arr = Peek(sp);

			m_gc.WriteBarrier(arr.GetPointer());
			MidoriArray& arr_ref = arr.GetPointer()->GetTraceable<MidoriArray>();
			arr_ref.AddBack(val);

			break;
		}
		MIDORI_HANDLER(GET_ARRAY_LENGTH)
		{
			MidoriValue arr = Pop(sp);
			MidoriArray& arr_ref = arr.GetPointer()->GetTraceable<MidoriArray>();
			MidoriInteger length = static_cast<MidoriInteger>(arr_ref.GetLength());
			Push(sp, length);
			break;
		}
		MIDORI_HANDLER(CREATE_INT_RANGE)
		{
			MidoriValue end = Pop(sp);
			MidoriValue step = Pop(sp);
			MidoriValue start = Pop(sp);

			MidoriIntRange range(start.GetInteger(), end.GetInteger(), step.GetInteger());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		MIDORI_HANDLER(CREATE_FLOAT_RANGE)
		{
			MidoriValue end = Pop(sp);
			MidoriValue step = Pop(sp);
			MidoriValue start = Pop(sp);

			MidoriFloatRange range(start.GetFloat(), end.GetFloat(), step.GetFloat());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		MIDORI_HANDLER(GET_RANGE_START)
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
		MIDORI_HANDLER(GET_RANGE_END)
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
		MIDORI_HANDLER(GET_RANGE_STEP)
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
		MIDORI_HANDLER(INT_TO_FLOAT)
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetInteger());
			break;
		}
		MIDORI_HANDLER(TEXT_TO_FLOAT)
		{
			const MidoriText& text = Peek(sp).GetPointer()->GetTraceable<MidoriText>();
			const std::optional<MidoriFloat> parsed = text.ParseFloat();
			if (!parsed.has_value())
			{
				m_instruction_pointer = inst_ip;
				m_value_stack_pointer = sp;
				m_value_stack_base_pointer = bp;
				m_curr_environment = env;
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidConversion, std::format("'{}' is not a Float.", text.View()), GetLine()));
			}
			Peek(sp) = parsed.value();
			break;
		}
		MIDORI_HANDLER(FLOAT_TO_INT)
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetFloat());
			break;
		}
		MIDORI_HANDLER(TEXT_TO_INT)
		{
			const MidoriText& text = Peek(sp).GetPointer()->GetTraceable<MidoriText>();
			const std::optional<MidoriInteger> parsed = text.ParseInteger();
			if (!parsed.has_value())
			{
				m_instruction_pointer = inst_ip;
				m_value_stack_pointer = sp;
				m_value_stack_base_pointer = bp;
				m_curr_environment = env;
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidConversion, std::format("'{}' is not an Int.", text.View()), GetLine()));
			}
			Peek(sp) = parsed.value();
			break;
		}
		MIDORI_HANDLER(FLOAT_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromFloat(Peek(sp).GetFloat()));
			break;
		}
		MIDORI_HANDLER(INT_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromInteger(Peek(sp).GetInteger()));
			break;
		}
		MIDORI_HANDLER(WORD_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MidoriText::FromWord(Peek(sp).GetWord()));
			break;
		}
		MIDORI_HANDLER(BYTE_TO_INT)
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetByte());
			break;
		}
		MIDORI_HANDLER(INT_TO_BYTE)
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetInteger() & 0xFF);
			break;
		}
		MIDORI_HANDLER(BYTE_TO_WORD)
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetByte());
			break;
		}
		MIDORI_HANDLER(WORD_TO_BYTE)
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetWord() & 0xFF);
			break;
		}
		MIDORI_HANDLER(WORD_TO_INT)
		{
			Peek(sp) = static_cast<MidoriInteger>(Peek(sp).GetWord());
			break;
		}
		MIDORI_HANDLER(INT_TO_WORD)
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetInteger());
			break;
		}
		MIDORI_HANDLER(BYTE_TO_FLOAT)
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetByte());
			break;
		}
		MIDORI_HANDLER(FLOAT_TO_BYTE)
		{
			Peek(sp) = static_cast<MidoriByte>(Peek(sp).GetFloat());
			break;
		}
		MIDORI_HANDLER(WORD_TO_FLOAT)
		{
			Peek(sp) = static_cast<MidoriFloat>(Peek(sp).GetWord());
			break;
		}
		MIDORI_HANDLER(FLOAT_TO_WORD)
		{
			Peek(sp) = static_cast<MidoriWord>(Peek(sp).GetFloat());
			break;
		}
		MIDORI_HANDLER(LEFT_SHIFT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftLeft(left.GetInteger(), static_cast<uint64_t>(right.GetInteger()));
			break;
		}
		MIDORI_HANDLER(RIGHT_SHIFT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftRight(left.GetInteger(), static_cast<uint64_t>(right.GetInteger()));

			break;
		}
		MIDORI_HANDLER(LEFT_SHIFT_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftLeft(left.GetByte(), right.GetByte());

			break;
		}
		MIDORI_HANDLER(RIGHT_SHIFT_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftRight(left.GetByte(), right.GetByte());

			break;
		}
		MIDORI_HANDLER(LEFT_SHIFT_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftLeft(left.GetWord(), right.GetWord());

			break;
		}
		MIDORI_HANDLER(RIGHT_SHIFT_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::ShiftRight(left.GetWord(), right.GetWord());

			break;
		}
		MIDORI_HANDLER(BITWISE_AND)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() & right.GetInteger();

			break;
		}
		MIDORI_HANDLER(BITWISE_OR)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() | right.GetInteger();

			break;
		}
		MIDORI_HANDLER(BITWISE_XOR)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() ^ right.GetInteger();

			break;
		}
		MIDORI_HANDLER(BITWISE_NOT)
		{
			MidoriValue& right = Peek(sp);

			right = ~right.GetInteger();

			break;
		}
		MIDORI_HANDLER(ADD_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() + right.GetFloat();

			break;
		}
		MIDORI_HANDLER(SUBTRACT_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() - right.GetFloat();

			break;
		}
		MIDORI_HANDLER(MULTIPLY_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() * right.GetFloat();

			break;
		}
		MIDORI_HANDLER(DIVIDE_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() / right.GetFloat();

			break;
		}
		MIDORI_HANDLER(MODULO_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = std::fmod(left.GetFloat(), right.GetFloat());

			break;
		}
		MIDORI_HANDLER(ADD_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::Add(left.GetInteger(), right.GetInteger());

			break;
		}
		MIDORI_HANDLER(SUBTRACT_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::Subtract(left.GetInteger(), right.GetInteger());

			break;
		}
		MIDORI_HANDLER(MULTIPLY_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::Multiply(left.GetInteger(), right.GetInteger());

			break;
		}
		MIDORI_HANDLER(DIVIDE_INTEGER)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::Divide(left.GetInteger(), right.GetInteger());

			break;
		}
		MIDORI_HANDLER(MODULO_INTEGER)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = MidoriIntegerArithmetic::Remainder(left.GetInteger(), right.GetInteger());

			break;
		}
		MIDORI_HANDLER(ADD_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() + right.GetByte());

			break;
		}
		MIDORI_HANDLER(SUBTRACT_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() - right.GetByte());

			break;
		}
		MIDORI_HANDLER(MULTIPLY_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() * right.GetByte());

			break;
		}
		MIDORI_HANDLER(DIVIDE_BYTE)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() / right.GetByte());

			break;
		}
		MIDORI_HANDLER(MODULO_BYTE)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = static_cast<MidoriByte>(left.GetByte() % right.GetByte());

			break;
		}
		MIDORI_HANDLER(ADD_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() + right.GetWord();

			break;
		}
		MIDORI_HANDLER(SUBTRACT_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() - right.GetWord();

			break;
		}
		MIDORI_HANDLER(MULTIPLY_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() * right.GetWord();

			break;
		}
		MIDORI_HANDLER(DIVIDE_WORD)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() / right.GetWord();

			break;
		}
		MIDORI_HANDLER(MODULO_WORD)
		{
			m_instruction_pointer = inst_ip;
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() % right.GetWord();

			break;
		}
		MIDORI_HANDLER(CONCAT_ARRAY)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			MidoriArray& left_value_vector_ref = left.GetPointer()->GetTraceable<MidoriArray>();
			MidoriArray& right_value_vector_ref = right.GetPointer()->GetTraceable<MidoriArray>();
			MidoriArray result = MidoriArray::Concatenate(left_value_vector_ref, right_value_vector_ref);

			left = AllocateTraceable(std::move(result));
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MIDORI_HANDLER(CONCAT_TEXT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			MidoriText& left_value_string_ref = left.GetPointer()->GetTraceable<MidoriText>();
			MidoriText& right_value_string_ref = right.GetPointer()->GetTraceable<MidoriText>();

			MidoriText result = MidoriText::Concatenate(left_value_string_ref, right_value_string_ref);

			left = AllocateTraceable(std::move(result));
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MIDORI_HANDLER(EXTEND_ARRAY)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			m_gc.WriteBarrier(left.GetPointer());
			left.GetPointer()->GetTraceable<MidoriArray>().Extend(right.GetPointer()->GetTraceable<MidoriArray>());

			break;
		}
		MIDORI_HANDLER(EXTEND_TEXT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left.GetPointer()->GetTraceable<MidoriText>().Append(right.GetPointer()->GetTraceable<MidoriText>());

			break;
		}
		MIDORI_HANDLER(ADD_LOCAL_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));

			MidoriValue& slot = *(bp + local_index);
			MidoriInteger result = MidoriIntegerArithmetic::Add(slot.GetInteger(), imm);
			slot = result;
			Push(sp, result);
			break;
		}
		MIDORI_HANDLER(PUSH_LOCAL_SUB_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));

			Push(sp, MidoriIntegerArithmetic::Subtract((bp + local_index)->GetInteger(), imm));
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_LE_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));
			int offset = ReadShort(ip);

			if (!((bp + local_index)->GetInteger() <= imm))
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_GE_LOCAL)
		{
			int left_index = static_cast<int>(ReadByte(ip));
			int right_index = static_cast<int>(ReadByte(ip));
			int offset = ReadShort(ip);

			if (!((bp + left_index)->GetInteger() >= (bp + right_index)->GetInteger()))
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(GET_LOCAL2)
		{
			int first_index = static_cast<int>(ReadByte(ip));
			int second_index = static_cast<int>(ReadByte(ip));

			Push(sp, *(bp + first_index));
			Push(sp, *(bp + second_index));
			break;
		}
		MIDORI_HANDLER(STORE_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			*(bp + offset) = Pop(sp);
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_TAG_NOT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			int tag = static_cast<int>(ReadByte(ip));
			int offset = ReadShort(ip);

			if ((bp + local_index)->GetPointer()->GetTraceable<MidoriUnion>().m_index != tag)
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(LOCAL_UNION_FIELD)
		{
			int union_index = static_cast<int>(ReadByte(ip));
			int field_index = static_cast<int>(ReadByte(ip));
			int target_index = static_cast<int>(ReadByte(ip));

			*(bp + target_index) = (bp + union_index)->GetPointer()->GetTraceable<MidoriUnion>().m_values[field_index];
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_LT_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));
			int offset = ReadShort(ip);

			if (!((bp + local_index)->GetInteger() < imm))
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_LT_LOCAL)
		{
			int left_index = static_cast<int>(ReadByte(ip));
			int right_index = static_cast<int>(ReadByte(ip));
			int offset = ReadShort(ip);

			if (!((bp + left_index)->GetInteger() < (bp + right_index)->GetInteger()))
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(IF_LOCAL_EQ_LOCAL)
		{
			int left_index = static_cast<int>(ReadByte(ip));
			int right_index = static_cast<int>(ReadByte(ip));
			int offset = ReadShort(ip);

			if (!((bp + left_index)->GetInteger() == (bp + right_index)->GetInteger()))
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(STEP_LOCAL)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MidoriInteger imm = static_cast<MidoriInteger>(static_cast<int8_t>(ReadByte(ip)));

			MidoriValue& slot = *(bp + local_index);
			slot = MidoriIntegerArithmetic::Add(slot.GetInteger(), imm);
			break;
		}
		MIDORI_HANDLER(LOCAL_ARRAY_GET)
		{
			MidoriArray& arr_ref = (bp + static_cast<int>(ReadByte(ip)))->GetPointer()->GetTraceable<MidoriArray>();
			MidoriValue& index = *(bp + static_cast<int>(ReadByte(ip)));
			int target_index = static_cast<int>(ReadByte(ip));
			m_instruction_pointer = inst_ip;

			int return_code = CheckIndexBounds(index, static_cast<MidoriInteger>(arr_ref.GetLength()));
			if (return_code != 0)
			{
				m_value_stack_pointer = sp;
				m_value_stack_base_pointer = bp;
				m_curr_environment = env;
				return return_code;
			}

			*(bp + target_index) = arr_ref[static_cast<int>(index.GetInteger())];
			break;
		}
		MIDORI_HANDLER(LOCAL_UNION2)
		{
			int tag = static_cast<int>(ReadByte(ip));
			const std::array<MidoriValue, 2uz> fields{ *(bp + static_cast<int>(ReadByte(ip))), *(bp + static_cast<int>(ReadByte(ip))) };
			int target_index = static_cast<int>(ReadByte(ip));

			*(bp + target_index) = AllocateTraceable(std::in_place_type<MidoriUnion>, std::span<const MidoriValue>(fields), tag);
			break;
		}
		MIDORI_HANDLER(APPEND_LOCAL)
		{
			MidoriTraceable* arr = (bp + static_cast<int>(ReadByte(ip)))->GetPointer();
			MidoriValue val = *(bp + static_cast<int>(ReadByte(ip)));

			m_gc.WriteBarrier(arr);
			arr->GetTraceable<MidoriArray>().AddBack(val);
			break;
		}
		MIDORI_HANDLER(EQUAL_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() == right.GetFloat();

			break;
		}
		MIDORI_HANDLER(NOT_EQUAL_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() != right.GetFloat();

			break;
		}
		MIDORI_HANDLER(GREATER_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() > right.GetFloat();

			break;
		}
		MIDORI_HANDLER(GREATER_EQUAL_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() >= right.GetFloat();

			break;
		}
		MIDORI_HANDLER(LESS_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() < right.GetFloat();

			break;
		}
		MIDORI_HANDLER(LESS_EQUAL_FLOAT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetFloat() <= right.GetFloat();

			break;
		}
		MIDORI_HANDLER(EQUAL_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() == right.GetInteger();

			break;
		}
		MIDORI_HANDLER(NOT_EQUAL_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() != right.GetInteger();

			break;
		}
		MIDORI_HANDLER(GREATER_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() > right.GetInteger();

			break;
		}
		MIDORI_HANDLER(GREATER_EQUAL_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() >= right.GetInteger();

			break;
		}
		MIDORI_HANDLER(LESS_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() < right.GetInteger();

			break;
		}
		MIDORI_HANDLER(LESS_EQUAL_INTEGER)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetInteger() <= right.GetInteger();

			break;
		}
		MIDORI_HANDLER(EQUAL_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() == right.GetByte();

			break;
		}
		MIDORI_HANDLER(NOT_EQUAL_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() != right.GetByte();

			break;
		}
		MIDORI_HANDLER(GREATER_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() > right.GetByte();

			break;
		}
		MIDORI_HANDLER(GREATER_EQUAL_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() >= right.GetByte();

			break;
		}
		MIDORI_HANDLER(LESS_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() < right.GetByte();

			break;
		}
		MIDORI_HANDLER(LESS_EQUAL_BYTE)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetByte() <= right.GetByte();

			break;
		}
		MIDORI_HANDLER(EQUAL_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() == right.GetWord();

			break;
		}
		MIDORI_HANDLER(NOT_EQUAL_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() != right.GetWord();

			break;
		}
		MIDORI_HANDLER(GREATER_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() > right.GetWord();

			break;
		}
		MIDORI_HANDLER(GREATER_EQUAL_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() >= right.GetWord();

			break;
		}
		MIDORI_HANDLER(LESS_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() < right.GetWord();

			break;
		}
		MIDORI_HANDLER(LESS_EQUAL_WORD)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetWord() <= right.GetWord();

			break;
		}
		MIDORI_HANDLER(EQUAL_TEXT)
		{
			MidoriValue right = Pop(sp);
			MidoriValue& left = Peek(sp);

			left = left.GetPointer()->GetTraceable<MidoriText>() == right.GetPointer()->GetTraceable<MidoriText>();

			break;
		}
		MIDORI_HANDLER(NOT)
		{
			MidoriValue& value = Peek(sp);
			value = !value.GetBool();
			break;
		}
		MIDORI_HANDLER(NEGATE_FLOAT)
		{
			MidoriValue& value = Peek(sp);
			value = -value.GetFloat();
			break;
		}
		MIDORI_HANDLER(NEGATE_INTEGER)
		{
			MidoriValue& value = Peek(sp);
			value = MidoriIntegerArithmetic::Negate(value.GetInteger());
			break;
		}
		MIDORI_HANDLER(JUMP_IF_FALSE)
		{
			MidoriValue value = Peek(sp);

			int offset = ReadShort(ip);
			if (!value.GetBool())
			{
				ip += offset;
			}
			break;
		}
		MIDORI_HANDLER(JUMP)
		{
			int offset = ReadShort(ip);
			ip += offset;
			break;
		}
		MIDORI_HANDLER(JUMP_BACK)
		{
			int offset = ReadShort(ip);
			ip -= offset;
			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MIDORI_HANDLER(IF_INTEGER_LESS)
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
		MIDORI_HANDLER(IF_INTEGER_LESS_EQUAL)
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
		MIDORI_HANDLER(IF_INTEGER_GREATER)
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
		MIDORI_HANDLER(IF_INTEGER_GREATER_EQUAL)
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
		MIDORI_HANDLER(IF_INTEGER_EQUAL)
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
		MIDORI_HANDLER(IF_INTEGER_NOT_EQUAL)
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
		MIDORI_HANDLER(IF_FLOAT_LESS)
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
		MIDORI_HANDLER(IF_FLOAT_LESS_EQUAL)
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
		MIDORI_HANDLER(IF_FLOAT_GREATER)
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
		MIDORI_HANDLER(IF_FLOAT_GREATER_EQUAL)
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
		MIDORI_HANDLER(IF_FLOAT_EQUAL)
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
		MIDORI_HANDLER(IF_FLOAT_NOT_EQUAL)
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
		MIDORI_HANDLER(GET_TAG)
		{
			MidoriValue union_val = Pop(sp);
			MidoriUnion& union_ref = union_val.GetPointer()->GetTraceable<MidoriUnion>();
			Push(sp, static_cast<MidoriInteger>(union_ref.m_index));
			break;
		}
		MIDORI_HANDLER(CALL_FOREIGN)
		{
			MidoriValue foreign_function_name = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));
			uint8_t return_type = static_cast<uint8_t>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!foreign_function_name.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalFFITypeError, std::format("Type error: expected function name (Text), but got {}.", foreign_function_name.ToText().View()), GetLine()));
			}
#endif

			MidoriText& foreign_function_name_ref = foreign_function_name.GetPointer()->GetTraceable<MidoriText>();

			VmFFIFunction proc = nullptr;
			std::optional<size_t> ffi_idx = MidoriFFIRegistry::FindIndex(foreign_function_name_ref.View());
			if (ffi_idx.has_value())
			{
				if (m_is_worker && ffi_idx.value() == MidoriFFIRegistry::ExitBuiltinIndex())
				{
					SyncMachineState(ip, sp, bp, env, closure);
					return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerExited, "Worker exited. Exiting the process from a worker would end the whole program, so the worker fails instead and the joiner receives Err(Failed(...)). A panic inside a worker takes this path.", GetLine()));
				}

				proc = m_ffi_table[ffi_idx.value()];
			}
			else
			{
				std::optional<VmFFIFunction> dynamic_func = m_dynamic_ffi_registry.FindFunction(foreign_function_name_ref.View());
				if (dynamic_func.has_value())
				{
					proc = dynamic_func.value();
				}
			}

			if (proc == nullptr)
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::FFIFunctionNotFound, std::format("Failed to load foreign function '{}'.", foreign_function_name_ref.View()), GetLine()));
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
						m_ffi_args[static_cast<size_t>(idx)] = (void*)ptr->GetTraceable<MidoriText>().CString();
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
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		MIDORI_HANDLER(CALL_FOREIGN_INDEXED)
		{
			uint8_t ffi_index = static_cast<uint8_t>(ReadByte(ip));
			int arity = static_cast<int>(ReadByte(ip));
			uint8_t return_type = static_cast<uint8_t>(ReadByte(ip));

			if (m_is_worker && static_cast<size_t>(ffi_index) == MidoriFFIRegistry::ExitBuiltinIndex())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerExited, "Worker exited. Exiting the process from a worker would end the whole program, so the worker fails instead and the joiner receives Err(Failed(...)). A panic inside a worker takes this path.", GetLine()));
			}

			const FFIEntry& ffi_entry = MidoriFFIRegistry::GetEntry(ffi_index);
			VmFFIFunction proc = m_ffi_table[ffi_index];

			m_ffi_array_args.clear();
			if (m_ffi_array_args.capacity() < static_cast<size_t>(arity))
			{
				m_ffi_array_args.reserve(static_cast<size_t>(arity));
			}
			for (int i = arity - 1; i >= 0; i -= 1)
			{
				size_t idx = static_cast<size_t>(i);
				MidoriValue arg = Pop(sp);
				const VmFFIArgumentKind arg_kind = ffi_entry.m_arg_kinds[idx];
				MidoriTraceable* ptr = arg.GetPointer();
				const bool is_managed_traceable = ptr != nullptr && m_gc.Contains(ptr);

				switch (arg_kind)
				{
				case VmFFIArgumentKind::CString:
					if (is_managed_traceable && ptr->IsTraceable<MidoriText>())
					{
						m_ffi_args[idx] = (void*)ptr->GetTraceable<MidoriText>().CString();
					}
					else
					{
						m_ffi_args[idx] = nullptr;
					}
					break;
				case VmFFIArgumentKind::ArrayView:
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
				case VmFFIArgumentKind::TraceableHandle:
					m_ffi_args[idx] = is_managed_traceable ? ptr : nullptr;
					break;
				case VmFFIArgumentKind::ValueHandle:
					m_ffi_value_args[idx] = arg;
					m_ffi_args[idx] = &m_ffi_value_args[idx];
					break;
				case VmFFIArgumentKind::RawValue:
				default:
					m_ffi_args[idx] = reinterpret_cast<void*>(static_cast<uintptr_t>(arg.GetRawBits()));
					break;
				}
			}

			MidoriValue return_val;
			proc(m_ffi_args.data(), reinterpret_cast<void*>(&return_val));

			VmFFIReturnKind return_kind = ffi_entry.m_return_kind;
			if (return_kind == VmFFIReturnKind::RawValue)
			{
				if (return_type == 1)
				{
					return_kind = VmFFIReturnKind::CString;
				}
				else if (return_type == 2)
				{
					return_kind = VmFFIReturnKind::ArrayValues;
				}
			}

			if (return_kind == VmFFIReturnKind::CString)
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
			else if (return_kind == VmFFIReturnKind::ArrayValues)
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
			else if (return_kind == VmFFIReturnKind::ArrayStrings)
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
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		MIDORI_HANDLER(CALL)
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			MidoriClosure& callee = closure->GetTraceable<MidoriClosure>();
			env = &callee.m_cell_values;

			ip = proc_entries[static_cast<size_t>(callee.m_proc_index)];
			bp = sp - arity;

			break;
		}
		MIDORI_HANDLER(CALL_0)
		MIDORI_HANDLER(CALL_1)
		MIDORI_HANDLER(CALL_2)
		MIDORI_HANDLER(CALL_3)
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(instruction) - static_cast<int>(VmOpCode::CALL_0);

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			MidoriClosure& callee = closure->GetTraceable<MidoriClosure>();
			env = &callee.m_cell_values;

			ip = proc_entries[static_cast<size_t>(callee.m_proc_index)];
			bp = sp - arity;

			break;
		}
		MIDORI_HANDLER(CALL_PROC_WIDE)
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			proc_index |= static_cast<int>(ReadByte(ip)) << 8;
			int arity = static_cast<int>(ReadByte(ip));

			PushCallFrame(csp, bp, ip, env, closure);

			env = nullptr;
			closure = nullptr;
			ip = proc_entries[static_cast<size_t>(proc_index)];
			bp = sp - arity;

			break;
		}
		MIDORI_HANDLER(CALL_GLOBAL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			int arity = static_cast<int>(ReadByte(ip));
			MidoriValue callable = (*m_global_vars)[global_idx];

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			MidoriClosure& callee = closure->GetTraceable<MidoriClosure>();
			env = &callee.m_cell_values;

			ip = proc_entries[static_cast<size_t>(callee.m_proc_index)];
			bp = sp - arity;

			break;
		}
		MIDORI_HANDLER(TAIL_CALL)
		{
			MidoriValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MIDORI_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Move arguments down to base pointer
			MidoriValue* args_source = sp - arity;
			if (arity > 0 && args_source != bp)
			{
				std::memmove(bp, args_source, arity * sizeof(MidoriValue));
			}
			sp = bp + arity;

			closure = callable.GetPointer();
			MidoriClosure& callee = closure->GetTraceable<MidoriClosure>();
			env = &callee.m_cell_values;

			// Jump to the start of the function without creating a new call frame
			ip = proc_entries[static_cast<size_t>(callee.m_proc_index)];

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		MIDORI_HANDLER(CONSTRUCT_STRUCT)
		{
			int size = static_cast<int>(ReadByte(ip));
			sp -= size;
			Push(sp, AllocateTraceable(std::in_place_type<MidoriStruct>, std::span<const MidoriValue>(sp, static_cast<size_t>(size))));
			break;
		}
		MIDORI_HANDLER(CONSTRUCT_UNION)
		{
			int size = static_cast<int>(ReadByte(ip));
			int tag = static_cast<int>(ReadByte(ip));
			sp -= size;
			Push(sp, AllocateTraceable(std::in_place_type<MidoriUnion>, std::span<const MidoriValue>(sp, static_cast<size_t>(size)), tag));
			break;
		}
		MIDORI_HANDLER(LOAD_EMPTY_UNION)
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
		MIDORI_HANDLER(MAKE_CLOSURE_OF)
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			proc_index |= static_cast<int>(ReadByte(ip)) << 8;
			int count = static_cast<int>(ReadByte(ip));

			// The captures stay on the stack, and so stay rooted, until the closure
			// holding them replaces them.
			ValueStackPointer captures = sp - count;
			MidoriTuple captured_cells(count);
			for (int i = 0; i < count; i += 1)
			{
				captured_cells[i] = AllocateTraceable(MidoriCellValue(captures[i]));
			}
			MidoriTraceable* made = AllocateTraceable(MidoriClosure{ .m_cell_values = std::move(captured_cells), .m_proc_index = proc_index });
			sp = captures;
			Push(sp, made);
			break;
		}
		MIDORI_HANDLER(SET_CAPTURE)
		{
			int index = static_cast<int>(ReadByte(ip));
			MidoriValue value = Pop(sp);
			MidoriTraceable* target = Pop(sp).GetPointer();
			MidoriTraceable* cell = AllocateTraceable(MidoriCellValue(value));
			m_gc.WriteBarrier(target);
			target->GetTraceable<MidoriClosure>().m_cell_values[index] = cell;
			break;
		}
		MIDORI_HANDLER(GET_UNION_FIELD)
		{
			int index = static_cast<int>(ReadByte(ip));
			MidoriValue& top = Peek(sp);
			top = top.GetPointer()->GetTraceable<MidoriUnion>().m_values[index];
			break;
		}
		MIDORI_HANDLER(MAKE_FUNCTION_WIDE)
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			proc_index |= static_cast<int>(ReadByte(ip)) << 8;

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
		MIDORI_HANDLER(GET_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			Push(sp, *(bp + offset));
			break;
		}
		MIDORI_HANDLER(SET_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			*(bp + offset) = Peek(sp);
			break;
		}
		MIDORI_HANDLER(GET_CELL)
		{
			int offset = static_cast<int>(ReadByte(ip));
#if MIDORI_DEBUG_FULL
			if (!env)
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, "GET_CELL called with null environment - function has captures but was called via CALL_PROC_WIDE", GetLine()));
			}
#endif
			MidoriValue cell_value = (*env)[offset].GetPointer()->GetTraceable<MidoriCellValue>().GetValue();
			Push(sp, cell_value);
			break;
		}
		MIDORI_HANDLER(MAKE_CELL)
		{
			// The initial value stays on the stack, and so stays rooted, until the
			// cell holding it replaces it.
			MidoriValue& top = Peek(sp);
			top = AllocateTraceable(MidoriMutableCell(top));
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MIDORI_HANDLER(READ_CELL)
		{
			MidoriValue& top = Peek(sp);
			top = top.GetPointer()->GetTraceable<MidoriMutableCell>().m_value;
			break;
		}
		MIDORI_HANDLER(WRITE_CELL)
		{
			MidoriValue value = Pop(sp);
			MidoriTraceable* cell = Pop(sp).GetPointer();
			m_gc.WriteBarrier(cell);
			cell->GetTraceable<MidoriMutableCell>().m_value = value;
			Push(sp, value);
			break;
		}
		MIDORI_HANDLER(DEFINE_GLOBAL_WIDE)
		{
			MidoriValue value = Pop(sp);
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = value;
			break;
		}
		MIDORI_HANDLER(GET_GLOBAL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			Push(sp, (*m_global_vars)[global_idx]);
			break;
		}
		MIDORI_HANDLER(SET_GLOBAL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int global_idx = (high_byte << 8) | low_byte;
			MidoriValue& var = (*m_global_vars)[global_idx];
			var = Peek(sp);
			break;
		}
		MIDORI_HANDLER(GET_LOCAL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			Push(sp, *(bp + offset));
			break;
		}
		MIDORI_HANDLER(SET_LOCAL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			*(bp + offset) = Peek(sp);
			break;
		}
		MIDORI_HANDLER(GET_CELL_WIDE)
		{
			int high_byte = static_cast<int>(ReadByte(ip));
			int low_byte = static_cast<int>(ReadByte(ip));
			int offset = (high_byte << 8) | low_byte;
			MidoriValue cell_value = (*env)[offset].GetPointer()->GetTraceable<MidoriCellValue>().GetValue();
			Push(sp, cell_value);
			break;
		}
		MIDORI_HANDLER(GET_MEMBER)
		{
			int index = static_cast<int>(ReadByte(ip));
			MidoriValue value = Pop(sp);
			Push(sp, value.GetPointer()->GetTraceable<MidoriStruct>().m_values[index]);
			break;
		}
		MIDORI_HANDLER(POP)
		{
			--sp;
			break;
		}
		MIDORI_HANDLER(RETURN)
		{
			MidoriValue value = Pop(sp);
			--csp;
			m_call_stack_pointer = csp;
			const CallFrame& frame = *csp;

			// Callee's bp points to where args started, which is our return point
			ValueStackPointer return_point = bp;

			bp = frame.m_return_bp;
			sp = return_point;
			ip = frame.m_return_ip;
			env = frame.m_closure_ptr;
			closure = frame.m_closure;

			Push(sp, value);

			break;
		}
		MIDORI_HANDLER(HALT)
		{
			SyncMachineState(ip, sp, bp, env, closure);
			return 0;
		}
		MIDORI_HANDLER(PUSH_PLACEHOLDER)
		{
			int count = static_cast<int>(ReadByte(ip));
			sp = std::fill_n(sp, count, MidoriValue());
			break;
		}
		MIDORI_HANDLER(SPAWN_WORKER)
		MIDORI_HANDLER(JOIN_WORKER)
		MIDORI_HANDLER(CHANNEL_CREATE)
		MIDORI_HANDLER(CHANNEL_SEND)
		MIDORI_HANDLER(CHANNEL_RECEIVE)
		MIDORI_HANDLER(CHANNEL_CLOSE)
		MIDORI_HANDLER(WORKER_IS_DONE)
		MIDORI_HANDLER(WORKER_CANCEL)
		{
			SyncMachineState(ip, sp, bp, env, closure);
			// Through a copy: once ip's address escapes, every dispatch stores it.
			InstructionPointer operands = ip;
			if (!ExecuteConcurrencyInstruction(instruction, operands))
			{
				return EXIT_FAILURE;
			}
			ip = operands;
			sp = m_value_stack_pointer;
			break;
		}
		default:
		{
			std::unreachable();
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

template<typename CommitPages>
static int CaptureExceptionFilter(EXCEPTION_POINTERS* ex_info, ExceptionInfo* out_info, CommitPages commit_pages)
{
	const DWORD exception_code = ex_info->ExceptionRecord->ExceptionCode;
	if (exception_code == EXCEPTION_ACCESS_VIOLATION && commit_pages(ex_info->ExceptionRecord->ExceptionInformation[1]))
	{
		return EXCEPTION_CONTINUE_EXECUTION;
	}

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
	__except (CaptureExceptionFilter(GetExceptionInformation(), &ex_info, [this](uintptr_t fault_address) -> bool { return CommitStackPages(fault_address); }))
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

// Commits the chunk from the page holding fault_address when that page lies in
// a stack region below its guard page; a fault on the guard page is an overflow.
bool VirtualMachine::CommitStackPages(uintptr_t fault_address) noexcept
{
	static constexpr size_t s_commit_chunk_size = 64uz * 1024uz;
	const std::array<std::pair<void*, size_t>, 2u> regions =
	{
		std::pair<void*, size_t>(m_value_stack_region, m_value_stack_region_size),
		std::pair<void*, size_t>(m_call_stack_region, m_call_stack_region_size),
	};

	return std::ranges::any_of(regions, [this, fault_address](const std::pair<void*, size_t>& region) -> bool
	{
		if (region.first == nullptr)
		{
			return false;
		}

		const uintptr_t region_begin = reinterpret_cast<uintptr_t>(region.first);
		const uintptr_t usable_end = region_begin + region.second - m_stack_page_size;
		if (fault_address < region_begin || fault_address >= usable_end)
		{
			return false;
		}

		const uintptr_t commit_begin = fault_address & ~(static_cast<uintptr_t>(m_stack_page_size) - 1u);
		const size_t commit_size = (std::min)(s_commit_chunk_size, static_cast<size_t>(usable_end - commit_begin));
		return VirtualAlloc(reinterpret_cast<void*>(commit_begin), commit_size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
	});
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
		LocateStackOverflowAtInnermostCall();
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
		LocateStackOverflowAtInnermostCall();
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




