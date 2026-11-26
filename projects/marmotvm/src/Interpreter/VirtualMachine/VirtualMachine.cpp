#include "Bytecode/Format/Format.h"
#include "Bytecode/Intrinsics/IntrinsicTable.h"
#include "Support/Attributes/Attributes.h"
#include "Support/Diagnostics/Diagnostics.h"
#include "Support/Terminal/Terminal.h"
#include "Bytecode/Scalar/FloatConversion.h"
#include "Bytecode/Scalar/IntegerArithmetic.h"
#include "Interpreter/Channel/Channel.h"
#include "Interpreter/ValueTransfer/ValueTransfer.h"
#include "Interpreter/Worker/BlockingProcessCall.h"
#include "Interpreter/Worker/CooperativeScheduler.h"
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
#include <mutex>
#include <numeric>
#include <print>
#include <ranges>
#include <unordered_map>

using namespace std::string_literals;

#if defined(_MSC_VER)
#define MARMOT_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define MARMOT_NOINLINE __attribute__((noinline))
#else
#define MARMOT_NOINLINE
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
#define MARMOT_THREADED_DISPATCH 1
#define MARMOT_HANDLER(name) case VmOpCode::name: handler_##name: __asm__ volatile("");
#else
#define MARMOT_THREADED_DISPATCH 0
#define MARMOT_HANDLER(name) case VmOpCode::name:
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
		const size_t separator_index = raw_view.rfind(MmcWire::MODULE_SEPARATOR);

		std::string_view display_name = raw_view;
		std::string_view module_name;
		if (separator_index != std::string_view::npos)
		{
			display_name = raw_view.substr(0u, separator_index);
			module_name = raw_view.substr(separator_index + 1u);
		}

		const bool is_module_bootstrap = display_name.starts_with(MmcWire::MODULE_BOOTSTRAP_PREFIX);
		if (display_name.starts_with(MmcWire::MAIN_PROCEDURE_PREFIX))
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
	};

	struct UnixSignalHandlers
	{
		std::mutex m_mutex;
		size_t m_users = 0u;
		struct sigaction m_previous_sigsegv {};
		struct sigaction m_previous_sigfpe {};
#if defined(SIGBUS)
		struct sigaction m_previous_sigbus {};
#endif
	};

	UnixSignalHandlers s_unix_signal_handlers;
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

	bool InstallVirtualMachineSignalHandlers()
	{
		UnixSignalHandlers& handlers = s_unix_signal_handlers;
		const std::lock_guard<std::mutex> lock(handlers.m_mutex);
		if (handlers.m_users != 0u)
		{
			++handlers.m_users;
			return true;
		}
		struct sigaction action {};
		std::memset(&action, 0, sizeof(action));
		sigemptyset(&action.sa_mask);
		action.sa_sigaction = HandleVirtualMachineSignal;
		action.sa_flags = SA_SIGINFO;

		if (sigaction(SIGSEGV, &action, &handlers.m_previous_sigsegv) != 0)
		{
			return false;
		}

		if (sigaction(SIGFPE, &action, &handlers.m_previous_sigfpe) != 0)
		{
			static_cast<void>(sigaction(SIGSEGV, &handlers.m_previous_sigsegv, nullptr));
			return false;
		}

#if defined(SIGBUS)
		if (sigaction(SIGBUS, &action, &handlers.m_previous_sigbus) != 0)
		{
			static_cast<void>(sigaction(SIGFPE, &handlers.m_previous_sigfpe, nullptr));
			static_cast<void>(sigaction(SIGSEGV, &handlers.m_previous_sigsegv, nullptr));
			return false;
		}
#endif
		++handlers.m_users;

		return true;
	}

	void RestoreVirtualMachineSignalHandlers()
	{
		UnixSignalHandlers& handlers = s_unix_signal_handlers;
		const std::lock_guard<std::mutex> lock(handlers.m_mutex);
		if (--handlers.m_users != 0u)
		{
			return;
		}
		static_cast<void>(sigaction(SIGSEGV, &handlers.m_previous_sigsegv, nullptr));
		static_cast<void>(sigaction(SIGFPE, &handlers.m_previous_sigfpe, nullptr));
#if defined(SIGBUS)
		static_cast<void>(sigaction(SIGBUS, &handlers.m_previous_sigbus, nullptr));
#endif
	}
#endif
}

VirtualMachine::VirtualMachine(VmExecutable&& executable) noexcept
	: m_owned_executable(std::make_shared<VmExecutable>(std::move(executable)))
{
	m_gc.SetAllocator(&m_allocator);
	m_executable = m_owned_executable.get();
	m_gc.SetShapes(&m_executable->GetShapes());
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

VirtualMachine::VirtualMachine(std::shared_ptr<const VmExecutable> shared_executable) noexcept
	: m_owned_executable(std::move(shared_executable))
	, m_is_worker(true)
{
	m_gc.SetAllocator(&m_allocator);
	m_executable = m_owned_executable.get();
	m_gc.SetShapes(&m_executable->GetShapes());

	m_owned_globals.resize(static_cast<size_t>(m_executable->GetGlobalVariableCount()));
	m_installed_globals.assign(m_owned_globals.size(), false);
	m_global_vars = &m_owned_globals;
	m_string_literal_cache.resize(m_executable->GetStringPool().size(), nullptr);

	InitializeProcEntryCache();
	InitializeStacks();
}

void VirtualMachine::PrepareWorkerCall(MarmotValue worker_function) noexcept
{
	m_value_stack_pointer = m_value_stack_begin;
	m_value_stack_base_pointer = m_value_stack_begin;
	m_call_stack_pointer = m_call_stack_begin;
	m_last_error.reset();

	// The spawned function is a transferred closure rather than a procedure index:
	// it is not on this VM's stack, so it is rooted here, and its cells are the
	// environment the worker's first frame runs in - which is what lets a lambda
	// that captured something be spawned.
	MarmotTraceable* closure = worker_function.GetPointer();
	m_curr_closure_traceable = closure;
	m_curr_environment = closure->GetValues();

	m_instruction_pointer = GetProcEntry(closure->GetIndex());

	PushCallFrame(m_value_stack_begin, &s_halt_bytecode[0], m_curr_environment, m_curr_closure_traceable);
	m_value_stack_base_pointer = m_value_stack_pointer;
}

void VirtualMachine::InstallGlobal(int global_index, MarmotValue value) noexcept
{
	const size_t slot = static_cast<size_t>(global_index);
	if (m_is_worker && !m_installed_globals[slot])
	{
		(*m_global_vars)[slot] = value;
		m_installed_globals[slot] = true;
	}
}

int VirtualMachine::ProcedureAt(const VmOpCode* ip) const noexcept
{
	const std::vector<ProcedureRange>::const_iterator after = std::ranges::upper_bound(m_procedure_ranges, ip, std::less<>{}, &ProcedureRange::m_begin);
	if (after == m_procedure_ranges.cbegin())
	{
		return -1;
	}
	const ProcedureRange& range = *std::prev(after);
	return ip < range.m_end ? range.m_proc_index : -1;
}

#if MARMOT_DEBUG_FULL
void VirtualMachine::KindMismatch(MarmotValue value, bool is_pointer, std::string_view where) const noexcept
{
	std::println(stderr, "internal error: {} holds {}, but the compiler says it holds {}.", where, value.ToText().View(), is_pointer ? "a pointer" : "a scalar");
	std::abort();
}

void VirtualMachine::CheckKinds(std::span<const MarmotValue> values, bool is_pointer, std::string_view where) const noexcept
{
	for (const MarmotValue value : values)
	{
		CheckKind(value, is_pointer, where);
	}
}

void VirtualMachine::CheckShape(std::span<const MarmotValue> values, uint16_t shape, std::string_view where) const noexcept
{
	const VmShape& pointer_fields = m_executable->GetShapes()[shape];
	if (!pointer_fields.empty() && pointer_fields.back() >= values.size())
	{
		std::println(stderr, "internal error: shape {} names field {} of {} with {} values.", shape, pointer_fields.back(), where, values.size());
		std::abort();
	}
	VmShape::const_iterator next_pointer = pointer_fields.cbegin();
	for (uint32_t index = 0u; index < values.size(); index += 1u)
	{
		const bool is_pointer = next_pointer != pointer_fields.cend() && *next_pointer == index;
		next_pointer += is_pointer ? 1 : 0;
		if (!values[index].AgreesWithKind(is_pointer))
		{
			KindMismatch(values[index], is_pointer, std::format("field {} of {} of shape {}", index, where, shape));
		}
	}
}

void VirtualMachine::CheckFrame(const VmOpCode* ip, const MarmotValue* sp, const MarmotValue* bp) const noexcept
{
	VisitFrame
	(
		ip,
		bp,
		sp,
		[this, ip, bp](const MarmotValue& value, bool is_pointer)
		{
			if (!value.AgreesWithKind(is_pointer))
			{
				const int proc_index = ProcedureAt(ip);
				KindMismatch(value, is_pointer, std::format("value {} of the frame of {} at offset {}", &value - bp, m_executable->m_procedure_names[static_cast<size_t>(proc_index)], ip - m_executable->GetBytecodeStream(proc_index)[0u]));
			}
		}
	);
}
#endif

// A frame is its slots, then the temporaries its safepoint lists, then, once a
// call has returned or an instruction has made it, the result. The VM's own
// halt, below the first procedure, has none.
template<typename Visit>
void VirtualMachine::VisitFrame(const VmOpCode* ip, const MarmotValue* bp, const MarmotValue* end, const Visit& visit) const noexcept
{
	const int proc_index = ProcedureAt(ip);
	if (proc_index < 0)
	{
		return;
	}
	const VmBytecodeStream& stream = m_executable->GetBytecodeStream(proc_index);
	const VmFrameMap& frame = stream.GetFrameMap();
	const int offset = static_cast<int>(ip - stream[0u]);
	const VmFrameMap::Safepoint* safepoint = frame.FindSafepoint(offset);
	const int slots = static_cast<int>(frame.m_slot_pointers.size());
	const int temporaries = safepoint != nullptr ? static_cast<int>(safepoint->m_temporaries.size()) : 0;
	const int region = static_cast<int>(end - bp);
	const bool fits = safepoint != nullptr ? (region == slots + temporaries || region == slots + temporaries + 1) : region <= slots;
	if (!fits)
	{
		std::println(stderr, "internal error: the frame of {} at offset {} holds {} values, but its map describes {} slots and {} temporaries.", m_executable->m_procedure_names[static_cast<size_t>(proc_index)], offset, region, slots, temporaries);
		std::abort();
	}
	for (int index = 0; index < (std::min)(region, slots); index += 1)
	{
		visit(bp[index], frame.m_slot_pointers[static_cast<size_t>(index)]);
	}
	for (int index = 0; index < temporaries; index += 1)
	{
		visit(bp[slots + index], safepoint->m_temporaries[static_cast<size_t>(index)]);
	}
	if (region == slots + temporaries + 1)
	{
		visit(bp[slots + temporaries], safepoint->m_result_is_pointer);
	}
}

MarmotValue VirtualMachine::MakeFunctionValue(int proc_index) noexcept
{
	const size_t cache_index = static_cast<size_t>(proc_index);
	if (cache_index < m_static_closure_cache.size() && m_static_closure_cache[cache_index] != nullptr)
	{
		return m_static_closure_cache[cache_index];
	}

	MarmotTraceable* closure = AllocateClosure(proc_index, {});
	if (cache_index < m_static_closure_cache.size())
	{
		m_static_closure_cache[cache_index] = closure;
	}

	return closure;
}

bool VirtualMachine::CheckWorkerOwner(MarmotInteger handle) noexcept
{
	if (m_worker_owner.Owns(handle))
	{
		return true;
	}
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidWorkerHandle, "Worker handle belongs to a different VM.", GetLine())));
	return false;
}

MARMOT_NOINLINE bool VirtualMachine::ExecuteConcurrencyInstruction(VmOpCode instruction, InstructionPointer& ip) noexcept
{
#ifdef MARMOT_WASM
	m_instruction_pointer = ip;
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::UnsupportedPlatformOperation, "Concurrency is not supported in the WebAssembly build.", GetLine())));
	return false;
#else
	CooperativeScheduler* const scheduler = m_worker_registry->GetScheduler();
	switch (instruction)
	{
	case VmOpCode::SPAWN_WORKER:
	{
		int arg_count = static_cast<int>(ReadByte(ip));
		const uint16_t crossing_shape = static_cast<uint16_t>(ReadShort(ip));
		const bool result_is_pointer = ReadByte(ip) != VmOpCode{ 0u };

		// The function is an ordinary value on the stack, above its arguments, and
		// crosses with its captures and arguments in one payload. Serialization also
		// follows the globals it can read, before the worker starts with its own heap.
		const size_t crossing_count = static_cast<size_t>(arg_count) + 1uz;
		m_value_stack_pointer -= crossing_count;
		const MarmotValue crossing = AllocateAggregate(MarmotTraceable::TraceableType::Tuple, std::span<const MarmotValue>(m_value_stack_pointer, crossing_count), 0, crossing_shape);

		std::expected<TransferPayload, std::string_view> payload = ValueTransfer::Serialize(crossing, true, *this);
		if (!payload.has_value())
		{
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidWorkerTransfer, payload.error(), GetLine())));
			return false;
		}
		const std::optional<MarmotInteger> worker_id = m_worker_registry->SpawnWorker(m_worker_owner, m_owned_executable, std::move(*payload), result_is_pointer);
		if (!worker_id.has_value())
		{
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidWorkerHandle, "Worker handle limit reached.", GetLine())));
			return false;
		}
		Push(*worker_id);
		return true;
	}
	case VmOpCode::JOIN_WORKER:
	{
		const uint16_t ok_shape = ReadByte(ip) != VmOpCode{ 0u } ? VM_FIRST_FIELD_SHAPE : VM_SCALAR_SHAPE;

		// The compiler checks Join's declared result by position: member 0 holds the
		// worker's value and member 1 why there is none, itself member 0 when the
		// worker was cancelled and member 1, with the message, when it failed. A
		// worker that failed is a value for the joining code to handle, not an end
		// to the joining VM.
		static constexpr int s_ok_tag = 0;
		static constexpr int s_err_tag = 1;
		static constexpr int s_cancelled_tag = 0;
		static constexpr int s_failed_tag = 1;
		const MarmotInteger worker_id = PeekValue().GetInteger();
		if (!CheckWorkerOwner(worker_id))
		{
			return false;
		}
		const std::optional<std::expected<TransferPayload, WorkerError>> joined = m_worker_registry->JoinWorkerValue(worker_id, m_scheduled_id, m_stop_token);
		if (!joined.has_value())
		{
			m_suspension = WorkerWait{ WorkerWait::Kind::Join, worker_id };
			return false;
		}
		Pop();
		const std::expected<TransferPayload, WorkerError>& worker_result = *joined;

		MarmotValue payload;
		int result_tag = s_ok_tag;
		uint16_t result_shape = ok_shape;
		if (worker_result.has_value())
		{
			payload = ValueTransfer::Deserialize(worker_result.value(), *this);
		}
		else
		{
			if (worker_result.error().m_cancelled)
			{
				payload = AllocateAggregate(MarmotTraceable::TraceableType::Union, {}, s_cancelled_tag, VM_SCALAR_SHAPE);
			}
			else
			{
				const MarmotValue message = AllocateTraceable(MarmotText(worker_result.error().m_message.c_str()));
				payload = AllocateAggregate(MarmotTraceable::TraceableType::Union, std::span<const MarmotValue>(&message, 1uz), s_failed_tag, VM_FIRST_FIELD_SHAPE);
			}
			result_tag = s_err_tag;
			result_shape = VM_FIRST_FIELD_SHAPE;
		}

		Push(AllocateAggregate(MarmotTraceable::TraceableType::Union, std::span<const MarmotValue>(&payload, 1uz), result_tag, result_shape));
		return true;
	}
	case VmOpCode::CHANNEL_CREATE:
	{
		const MarmotInteger capacity = Pop().GetInteger();
		const int channel_id = scheduler == nullptr ? ChannelRegistry::GetInstance().CreateChannel(capacity) : scheduler->CreateChannel(capacity);
		Push(static_cast<MarmotInteger>(channel_id));
		return true;
	}
	case VmOpCode::CHANNEL_SEND:
	{
		const bool value_is_pointer = ReadByte(ip) != VmOpCode{ 0u };
		const int channel_id = static_cast<int>(m_value_stack_pointer[-2].GetInteger());
		if (!m_pending_send.has_value())
		{
			std::expected<TransferPayload, std::string_view> payload = ValueTransfer::Serialize(PeekValue(), value_is_pointer, *this);
			if (!payload.has_value())
			{
				static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidWorkerTransfer, payload.error(), GetLine())));
				return false;
			}
			m_pending_send = std::move(*payload);
		}
		const ChannelOpStatus send_status = scheduler == nullptr
			? ChannelRegistry::GetInstance().Send(channel_id, std::move(*m_pending_send), m_stop_token)
			: scheduler->Send(m_scheduled_id, channel_id, std::move(*m_pending_send), m_stop_token);
		if (send_status == ChannelOpStatus::WouldBlock)
		{
			m_suspension = WorkerWait{ WorkerWait::Kind::ChannelSend, channel_id };
			return false;
		}
		m_pending_send.reset();
		m_value_stack_pointer -= 2;
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
		static constexpr int s_none_tag = 0;
		static constexpr int s_some_tag = 1;
		const uint16_t payload_shape = ReadByte(ip) != VmOpCode{ 0u } ? VM_FIRST_FIELD_SHAPE : VM_SCALAR_SHAPE;
		const int channel_id = static_cast<int>(PeekValue().GetInteger());
		const ChannelReceiveResult received_value = scheduler == nullptr
			? ChannelRegistry::GetInstance().Receive(channel_id, m_stop_token)
			: scheduler->Receive(m_scheduled_id, channel_id, m_stop_token);
		if (received_value.m_status == ChannelOpStatus::WouldBlock)
		{
			m_suspension = WorkerWait{ WorkerWait::Kind::ChannelReceive, channel_id };
			return false;
		}
		Pop();
		if (received_value.m_status == ChannelOpStatus::Cancelled)
		{
			static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine())));
			return false;
		}
		if (received_value.m_status == ChannelOpStatus::Closed)
		{
			Push(EmptyUnion(s_none_tag));
			return true;
		}

		const MarmotValue value = ValueTransfer::Deserialize(received_value.m_value.value(), *this);
		Push(AllocateAggregate(MarmotTraceable::TraceableType::Union, std::span<const MarmotValue>(&value, 1uz), s_some_tag, payload_shape));
		return true;
	}
	case VmOpCode::CHANNEL_CLOSE:
	{
		const int channel_id = static_cast<int>(Pop().GetInteger());
		if (scheduler != nullptr)
		{
			scheduler->Close(channel_id);
		}
		else
		{
			ChannelRegistry::GetInstance().Close(channel_id);
		}
		Push(MarmotValue());
		return true;
	}
	case VmOpCode::WORKER_IS_DONE:
	{
		const MarmotInteger worker_id = Pop().GetInteger();
		if (!CheckWorkerOwner(worker_id))
		{
			return false;
		}
		const bool is_done = m_worker_registry->IsWorkerDone(worker_id);
		Push(is_done);
		return true;
	}
	case VmOpCode::WORKER_CANCEL:
	{
		const MarmotInteger worker_id = Pop().GetInteger();
		if (!CheckWorkerOwner(worker_id))
		{
			return false;
		}
		const bool cancelled = m_worker_registry->CancelWorker(worker_id);
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
	m_capture_shapes.resize(static_cast<size_t>(count));
	m_static_closure_cache.assign(static_cast<size_t>(count), nullptr);
	for (int i = 0; i < count; i += 1)
	{
		const VmBytecodeStream& bytecode = m_executable->GetBytecodeStream(i);
		m_proc_entry_cache[static_cast<size_t>(i)] = bytecode[0u];
		m_capture_shapes[static_cast<size_t>(i)] = bytecode.GetFrameMap().m_capture_shape;
		m_procedure_ranges.push_back(ProcedureRange{ bytecode[0u], bytecode[0u] + bytecode.GetByteCodeSize(), i });
	}
	std::ranges::sort(m_procedure_ranges, std::less<>{}, &ProcedureRange::m_begin);
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

	const size_t value_stack_bytes = s_value_stack_size * sizeof(MarmotValue);
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
		m_value_stack_begin = reinterpret_cast<MarmotValue*>(value_region + (value_usable_bytes - value_stack_bytes));
		m_value_stack_region_size = value_total_size;
		static_cast<void>(CommitStackPages(reinterpret_cast<uintptr_t>(m_value_stack_begin)));
	}
	else
	{
		m_value_stack_begin = static_cast<MarmotValue*>(std::malloc(value_stack_bytes));
	}
#else
	m_value_stack_region = mmap(nullptr, value_total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m_value_stack_region != MAP_FAILED)
	{
		char* value_region = static_cast<char*>(m_value_stack_region);
		m_value_stack_begin = reinterpret_cast<MarmotValue*>(value_region + (value_usable_bytes - value_stack_bytes));
		m_value_stack_region_size = value_total_size;
		static_cast<void>(mprotect(value_region + value_usable_bytes, m_stack_page_size, PROT_NONE));
	}
	else
	{
		m_value_stack_region = nullptr;
		m_value_stack_begin = static_cast<MarmotValue*>(std::malloc(value_stack_bytes));
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

	const uintptr_t value_guard_begin = reinterpret_cast<uintptr_t>(m_value_stack_begin) + (s_value_stack_size * sizeof(MarmotValue));
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

int VirtualMachine::IndexOutOfBounds(MarmotInteger index, InstructionPointer ip, ValueStackPointer sp, ValueStackPointer bp, MarmotValue* env, MarmotTraceable* closure) noexcept
{
	SyncMachineState(ip, sp, bp, env, closure);
	return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::IndexOutOfBounds, std::format("Index out of bounds at index: {}.", index), GetLine()));
}

MarmotTraceable* VirtualMachine::EmptyUnion(int tag) noexcept
{
	MarmotTraceable*& cached = m_empty_union_cache[static_cast<size_t>(tag)];
	if (cached == nullptr)
	{
		cached = AllocateAggregate(MarmotTraceable::TraceableType::Union, {}, tag, VM_SCALAR_SHAPE);
	}
	return cached;
}

int VirtualMachine::InvalidFloatConversion(MarmotFloat value, std::string_view destination) noexcept
{
	const MarmotText text = MarmotText::FromFloat(value);
	return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidConversion, std::format("Float '{}' cannot be converted to {}.", text.View(), destination), GetLine()));
}

// The result stays in the library's allocator until the VM has copied it.
MarmotValue VirtualMachine::TakeNativeResult(uint8_t return_tag, MarmotWord bits, MarmotFFIFreeResultFn free_result) noexcept
{
	static_assert(MmcWire::FFI_RETURN_RAW_VALUE == MARMOT_FFI_RETURN_SCALAR);
	static_assert(MmcWire::FFI_RETURN_C_STRING == MARMOT_FFI_RETURN_STRING);
	static_assert(MmcWire::FFI_RETURN_ARRAY_VALUES == MARMOT_FFI_RETURN_ARRAY_VALUES);
	static_assert(MmcWire::FFI_RETURN_ARRAY_STRINGS == MARMOT_FFI_RETURN_ARRAY_STRINGS);

	const uint8_t kind = return_tag & MmcWire::FFI_RETURN_KIND_MASK;
	if (kind == MARMOT_FFI_RETURN_SCALAR)
	{
		return MarmotValue::FromRawBits(bits);
	}
	const auto release = [free_result, kind](void* ptr) { free_result(kind, ptr); };
	const std::unique_ptr<void, decltype(release)> result(reinterpret_cast<void*>(static_cast<uintptr_t>(bits)), release);
	if (kind == MARMOT_FFI_RETURN_STRING)
	{
		return AllocateTraceable(MarmotText(result ? static_cast<const char*>(result.get()) : ""));
	}

	const auto* array = static_cast<const MarmotFFIArray*>(result.get());
	const int32_t length = array == nullptr ? 0 : array->length;
	MarmotArray copy(length);
	if (kind == MARMOT_FFI_RETURN_ARRAY_STRINGS)
	{
		for (int32_t index = 0; index < length; index += 1)
		{
			const char* text = static_cast<char* const*>(array->data)[index];
			copy[index] = AllocateTraceable(MarmotText(text == nullptr ? "" : text));
		}
		return AllocateArray(std::move(copy), true);
	}
	for (int32_t index = 0; index < length; index += 1)
	{
		MarmotWord element = 0;
		std::memcpy(&element, static_cast<const char*>(array->data) + index * sizeof(element), sizeof(element));
		copy[index] = MarmotValue::FromRawBits(element);
	}
	return AllocateArray(std::move(copy), false);
}

MARMOT_FORCE_INLINE bool VirtualMachine::PassForeignArgument(uint8_t kind, size_t index, MarmotValue argument) noexcept
{
	MarmotTraceable* pointer = argument.GetPointer();
#if MARMOT_DEBUG_FULL
	const bool is_array = kind == MmcWire::FFI_ARGUMENT_ARRAY_VIEW || kind == MmcWire::FFI_ARGUMENT_ARRAY_STRINGS;
	const bool is_object = kind == MmcWire::FFI_ARGUMENT_C_STRING || is_array;
	const bool is_its_kind = !is_object || (pointer != nullptr
		&& (kind != MmcWire::FFI_ARGUMENT_C_STRING || pointer->IsTraceable<MarmotText>())
		&& (!is_array || pointer->IsTraceable<MarmotArray>()));
	if (!is_its_kind)
	{
		KindMismatch(argument, true, std::format("argument {} of a foreign call", index));
	}
#endif
	switch (kind)
	{
	case MmcWire::FFI_ARGUMENT_C_STRING:
		m_ffi_args[index] = (void*)pointer->GetTraceable<MarmotText>().CString();
		return m_ffi_args[index] != nullptr;
	case MmcWire::FFI_ARGUMENT_ARRAY_VIEW:
	{
		const MarmotArray& array = pointer->GetTraceable<MarmotArray>();
		std::vector<MarmotWord>& slots = m_ffi_scalar_arrays[index];
		slots.resize(array.GetLength());
		for (int element = 0; element < array.GetLength(); element += 1)
		{
			slots[element] = array[element].GetRawBits();
		}
		m_ffi_array_args[index] = MarmotFFIArray{slots.data(), array.GetLength()};
		m_ffi_args[index] = &m_ffi_array_args[index];
		break;
	}
	case MmcWire::FFI_ARGUMENT_ARRAY_STRINGS:
	{
		const MarmotArray& array = pointer->GetTraceable<MarmotArray>();
		std::vector<const char*>& strings = m_ffi_string_arrays[index];
		strings.resize(array.GetLength());
		for (int element = 0; element < array.GetLength(); element += 1)
		{
			strings[element] = array[element].GetPointer()->GetTraceable<MarmotText>().CString();
			if (strings[element] == nullptr)
			{
				return false;
			}
		}
		m_ffi_array_args[index] = MarmotFFIArray{strings.data(), array.GetLength()};
		m_ffi_args[index] = &m_ffi_array_args[index];
		break;
	}
	case MmcWire::FFI_ARGUMENT_RAW_VALUE:
		m_ffi_args[index] = reinterpret_cast<void*>(static_cast<uintptr_t>(argument.GetRawBits()));
		break;
	}
	return true;
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

	// The top frame runs to the stack pointer, and each one below it, suspended
	// at a call, to the base of the frame it called.
	const auto add_pointers = [&roots](const MarmotValue& value, bool is_pointer)
	{
		if (is_pointer && value.GetPointer() != nullptr)
		{
			roots.emplace_back(value.GetPointer());
		}
	};
	if (stack_count > 0uz)
	{
		const MarmotValue* end = m_value_stack_pointer;
		const MarmotValue* bp = m_value_stack_base_pointer;
		InstructionPointer ip = m_instruction_pointer;
		for (CallStackPointer frame = m_call_stack_pointer; ; )
		{
			VisitFrame(ip, bp, end, add_pointers);
			if (frame == m_call_stack_begin)
			{
				break;
			}
			frame -= 1;
			end = bp;
			bp = frame->m_return_bp;
			ip = frame->m_return_ip;
		}
	}

	if (m_global_vars != nullptr)
	{
		for (size_t index = 0uz; index < m_global_vars->size(); index += 1uz)
		{
			add_pointers((*m_global_vars)[index], m_executable->IsGlobalPointer(static_cast<int>(index)));
		}
	}

	if (m_curr_closure_traceable != nullptr)
	{
		roots.emplace_back(m_curr_closure_traceable);
	}

	for (CallStackPointer frame = m_call_stack_begin; frame != m_call_stack_pointer; ++frame)
	{
		if (frame->m_closure != nullptr)
		{
			roots.emplace_back(frame->m_closure);
		}
	}

	for (MarmotTraceable* cached_string : m_string_literal_cache)
	{
		if (cached_string)
		{
			roots.emplace_back(cached_string);
		}
	}

	for (MarmotTraceable* cached_union : m_empty_union_cache)
	{
		if (cached_union)
		{
			roots.emplace_back(cached_union);
		}
	}

	// A cached function is handed out again by MAKE_FUNCTION_WIDE, so it must
	// outlive every value that held it.
	for (MarmotTraceable* cached_function : m_static_closure_cache)
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
	// Select once so scheduler checkpoints cannot change native dispatch.
	if (m_worker_registry->GetScheduler() == nullptr)
	{
		return ExecuteInstructions<false>();
	}
	return ExecuteInstructions<true>();
}

template<bool Cooperative>
int VirtualMachine::ExecuteInstructions() noexcept
{
	InstructionPointer ip = m_instruction_pointer;
	ValueStackPointer sp = m_value_stack_pointer;
	ValueStackPointer bp = m_value_stack_base_pointer;
	MarmotValue* env = m_curr_environment;
	MarmotTraceable* closure = m_curr_closure_traceable;
	CallStackPointer csp = m_call_stack_pointer;
	const InstructionPointer* const proc_entries = m_proc_entry_cache.data();
	unsigned remaining_checkpoints = 1024u;
	if constexpr (Cooperative)
	{
		if (m_pending_process != nullptr)
		{
			const MarmotWord bits = static_cast<MarmotWord>(reinterpret_cast<uintptr_t>(m_pending_process->ReleaseResult()));
			Push(sp, TakeNativeResult(MARMOT_FFI_RETURN_ARRAY_STRINGS, bits, m_pending_process->GetFreeResult()));
			m_pending_process.reset();
		}
	}

#if MARMOT_ENABLE_OPCODE_METRICS
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
				std::print(RuntimeDiagnostics::Output(), "Opcode metrics ({}):\n", Cooperative ? "this VM slice" : "this VM");
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

#if MARMOT_THREADED_DISPATCH
	static const std::array<void*, VmOpCodeTable::COUNT> s_dispatch_table =
	{
#define MARMOT_OPCODE(name, length) &&handler_##name,
#include "Bytecode/Executable/OpCodes.def"
#undef MARMOT_OPCODE
	};
#endif

	while (true)
	{

#if MARMOT_ENABLE_EXECUTION_TRACE
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
#if MARMOT_ENABLE_OPCODE_METRICS
		if (opcode_metrics)
		{
			opcode_counts[static_cast<size_t>(instruction)] += 1u;
		}
#endif

#if MARMOT_THREADED_DISPATCH
		goto *s_dispatch_table[static_cast<size_t>(instruction)];
#endif
		switch (instruction)
		{
		MARMOT_HANDLER(LOAD_STRING_WIDE)
		{
			size_t index = static_cast<size_t>(ReadShort(ip));
			if (index >= m_string_literal_cache.size() || !m_string_literal_cache[index])
			{
				if (index >= m_string_literal_cache.size())
				{
					m_string_literal_cache.resize(index + 1, nullptr);
				}
				m_string_literal_cache[index] = AllocateTraceable(MarmotText(m_executable->GetStringPool()[index]));
			}
			// Shared by every load: lowering never extends a text literal in place.
			Push(sp, m_string_literal_cache[index]);
			break;
		}
		MARMOT_HANDLER(INTEGER_CONSTANT)
		{
			Push(sp, ReadIntegerConstant(ip));
			break;
		}
		MARMOT_HANDLER(FLOAT_CONSTANT)
		{
			Push(sp, ReadFloatConstant(ip));
			break;
		}
		MARMOT_HANDLER(BYTE_CONSTANT)
		{
			Push(sp, ReadByteConstant(ip));
			break;
		}
		MARMOT_HANDLER(WORD_CONSTANT)
		{
			Push(sp, ReadWordConstant(ip));
			break;
		}
		MARMOT_HANDLER(OP_UNIT)
		{
			Push(sp, MarmotValue());
			break;
		}
		MARMOT_HANDLER(OP_TRUE)
		{
			Push(sp, true);
			break;
		}
		MARMOT_HANDLER(OP_FALSE)
		{
			Push(sp, false);
			break;
		}
		MARMOT_HANDLER(INT_MINUS_1)
		{
			Push(sp, MarmotInteger{-1});
			break;
		}
		MARMOT_HANDLER(INT_0)
		{
			Push(sp, MarmotInteger{0});
			break;
		}
		MARMOT_HANDLER(INT_1)
		{
			Push(sp, MarmotInteger{1});
			break;
		}
		MARMOT_HANDLER(INT_2)
		{
			Push(sp, MarmotInteger{2});
			break;
		}
		MARMOT_HANDLER(INT_3)
		{
			Push(sp, MarmotInteger{3});
			break;
		}
		MARMOT_HANDLER(INT_4)
		{
			Push(sp, MarmotInteger{4});
			break;
		}
		MARMOT_HANDLER(INT_5)
		{
			Push(sp, MarmotInteger{5});
			break;
		}
		MARMOT_HANDLER(INT_10)
		{
			Push(sp, MarmotInteger{10});
			break;
		}
		MARMOT_HANDLER(CREATE_ARRAY)
		{
			int count = ReadThreeBytes(ip);
			const bool holds_pointers = ReadByte(ip) != VmOpCode{ 0u };
			MarmotArray arr(count);

			for (int i = count - 1; i >= 0; i -= 1)
			{
				arr[i] = Pop(sp);
			}

			Push(sp, AllocateArray(std::move(arr), holds_pointers));
			break;
		}
		MARMOT_HANDLER(CREATE_TUPLE)
		{
			int count = ReadThreeBytes(ip);
			const uint16_t shape = static_cast<uint16_t>(ReadShort(ip));
			sp -= count;
			Push(sp, AllocateAggregate(MarmotTraceable::TraceableType::Tuple, std::span<const MarmotValue>(sp, static_cast<size_t>(count)), 0, shape));
			break;
		}
		MARMOT_HANDLER(GET_ARRAY)
		{
			const MarmotInteger index = (sp - 1)->GetInteger();
			MarmotValue* arr_slot = sp - 2;
			MarmotValue arr = *arr_slot;
			MarmotArray& arr_ref = arr.GetPointer()->GetTraceable<MarmotArray>();

			if (IsIndexOutOfBounds(index, arr_ref.GetLength())) [[unlikely]]
			{
				return IndexOutOfBounds(index, inst_ip, arr_slot, bp, env, closure);
			}

			*arr_slot = arr_ref[static_cast<int>(index)];
			sp = arr_slot + 1;

			break;
		}
		MARMOT_HANDLER(GET_TUPLE)
		{
			const MarmotInteger index = (sp - 1)->GetInteger();
			MarmotValue* tuple_slot = sp - 2;
			MarmotValue tuple_value = *tuple_slot;
			MarmotTraceable* tuple_ref = tuple_value.GetPointer();

			if (IsIndexOutOfBounds(index, tuple_ref->GetLength())) [[unlikely]]
			{
				return IndexOutOfBounds(index, inst_ip, tuple_slot, bp, env, closure);
			}

			*tuple_slot = tuple_ref->GetValues()[static_cast<size_t>(index)];
			sp = tuple_slot + 1;

			break;
		}
		MARMOT_HANDLER(ADD_BACK_ARRAY)
		{
			MarmotValue val = Pop(sp);
			MarmotTraceable* arr = Peek(sp).GetPointer();
			const bool holds_pointers = arr->HoldsPointers();

#if MARMOT_DEBUG_FULL
			CheckKind(val, holds_pointers, "an array appended to");
#endif
			if (holds_pointers)
			{
				m_gc.WriteBarrier(arr);
			}
			MarmotArray& arr_ref = arr->GetTraceable<MarmotArray>();
			arr_ref.AddBack(val);

			break;
		}
		MARMOT_HANDLER(GET_ARRAY_LENGTH)
		{
			MarmotValue arr = Pop(sp);
			MarmotArray& arr_ref = arr.GetPointer()->GetTraceable<MarmotArray>();
			MarmotInteger length = static_cast<MarmotInteger>(arr_ref.GetLength());
			Push(sp, length);
			break;
		}
		MARMOT_HANDLER(CREATE_INT_RANGE)
		{
			MarmotValue end = Pop(sp);
			MarmotValue step = Pop(sp);
			MarmotValue start = Pop(sp);

			MarmotIntRange range(start.GetInteger(), end.GetInteger(), step.GetInteger());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		MARMOT_HANDLER(CREATE_FLOAT_RANGE)
		{
			MarmotValue end = Pop(sp);
			MarmotValue step = Pop(sp);
			MarmotValue start = Pop(sp);

			MarmotFloatRange range(start.GetFloat(), end.GetFloat(), step.GetFloat());

			Push(sp, AllocateTraceable(std::move(range)));
			break;
		}
		MARMOT_HANDLER(GET_RANGE_START)
		{
			MarmotValue range_ptr = Pop(sp);
			MarmotTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MarmotIntRange>())
			{
				Push(sp, ptr->GetTraceable<MarmotIntRange>().GetStart());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MarmotFloatRange>().GetStart());
			}
			break;
		}
		MARMOT_HANDLER(GET_RANGE_END)
		{
			MarmotValue range_ptr = Pop(sp);
			MarmotTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MarmotIntRange>())
			{
				Push(sp, ptr->GetTraceable<MarmotIntRange>().GetEnd());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MarmotFloatRange>().GetEnd());
			}
			break;
		}
		MARMOT_HANDLER(GET_RANGE_STEP)
		{
			MarmotValue range_ptr = Pop(sp);
			MarmotTraceable* ptr = range_ptr.GetPointer();
			if (ptr->IsTraceable<MarmotIntRange>())
			{
				Push(sp, ptr->GetTraceable<MarmotIntRange>().GetStep());
			}
			else
			{
				Push(sp, ptr->GetTraceable<MarmotFloatRange>().GetStep());
			}
			break;
		}
		MARMOT_HANDLER(INT_TO_FLOAT)
		{
			Peek(sp) = static_cast<MarmotFloat>(Peek(sp).GetInteger());
			break;
		}
		MARMOT_HANDLER(TEXT_TO_FLOAT)
		{
			const MarmotText& text = Peek(sp).GetPointer()->GetTraceable<MarmotText>();
			const std::optional<MarmotFloat> parsed = text.ParseFloat();
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
		MARMOT_HANDLER(FLOAT_TO_INT)
		{
			const MarmotFloat value = Peek(sp).GetFloat();
			const std::optional<MarmotInteger> converted = MarmotFloatConversion::ToInteger<MarmotInteger>(value);
			if (!converted.has_value())
			{
				SyncMachineState(inst_ip, sp, bp, env, closure);
				return InvalidFloatConversion(value, "Int");
			}
			Peek(sp) = *converted;
			break;
		}
		MARMOT_HANDLER(TEXT_TO_INT)
		{
			const MarmotText& text = Peek(sp).GetPointer()->GetTraceable<MarmotText>();
			const std::optional<MarmotInteger> parsed = text.ParseInteger();
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
		MARMOT_HANDLER(FLOAT_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MarmotText::FromFloat(Peek(sp).GetFloat()));
			break;
		}
		MARMOT_HANDLER(INT_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MarmotText::FromInteger(Peek(sp).GetInteger()));
			break;
		}
		MARMOT_HANDLER(WORD_TO_TEXT)
		{
			Peek(sp) = AllocateTraceable(MarmotText::FromWord(Peek(sp).GetWord()));
			break;
		}
		MARMOT_HANDLER(BYTE_TO_INT)
		{
			Peek(sp) = static_cast<MarmotInteger>(Peek(sp).GetByte());
			break;
		}
		MARMOT_HANDLER(INT_TO_BYTE)
		{
			Peek(sp) = static_cast<MarmotByte>(Peek(sp).GetInteger() & 0xFF);
			break;
		}
		MARMOT_HANDLER(BYTE_TO_WORD)
		{
			Peek(sp) = static_cast<MarmotWord>(Peek(sp).GetByte());
			break;
		}
		MARMOT_HANDLER(WORD_TO_BYTE)
		{
			Peek(sp) = static_cast<MarmotByte>(Peek(sp).GetWord() & 0xFF);
			break;
		}
		MARMOT_HANDLER(WORD_TO_INT)
		{
			Peek(sp) = static_cast<MarmotInteger>(Peek(sp).GetWord());
			break;
		}
		MARMOT_HANDLER(INT_TO_WORD)
		{
			Peek(sp) = static_cast<MarmotWord>(Peek(sp).GetInteger());
			break;
		}
		MARMOT_HANDLER(BYTE_TO_FLOAT)
		{
			Peek(sp) = static_cast<MarmotFloat>(Peek(sp).GetByte());
			break;
		}
		MARMOT_HANDLER(FLOAT_TO_BYTE)
		{
			const MarmotFloat value = Peek(sp).GetFloat();
			const std::optional<MarmotByte> converted = MarmotFloatConversion::ToInteger<MarmotByte>(value);
			if (!converted.has_value())
			{
				SyncMachineState(inst_ip, sp, bp, env, closure);
				return InvalidFloatConversion(value, "Byte");
			}
			Peek(sp) = *converted;
			break;
		}
		MARMOT_HANDLER(WORD_TO_FLOAT)
		{
			Peek(sp) = static_cast<MarmotFloat>(Peek(sp).GetWord());
			break;
		}
		MARMOT_HANDLER(FLOAT_TO_WORD)
		{
			const MarmotFloat value = Peek(sp).GetFloat();
			const std::optional<MarmotWord> converted = MarmotFloatConversion::ToInteger<MarmotWord>(value);
			if (!converted.has_value())
			{
				SyncMachineState(inst_ip, sp, bp, env, closure);
				return InvalidFloatConversion(value, "Word");
			}
			Peek(sp) = *converted;
			break;
		}
		MARMOT_HANDLER(LEFT_SHIFT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftLeft(left.GetInteger(), static_cast<uint64_t>(right.GetInteger()));
			break;
		}
		MARMOT_HANDLER(RIGHT_SHIFT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftRight(left.GetInteger(), static_cast<uint64_t>(right.GetInteger()));

			break;
		}
		MARMOT_HANDLER(LEFT_SHIFT_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftLeft(left.GetByte(), right.GetByte());

			break;
		}
		MARMOT_HANDLER(RIGHT_SHIFT_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftRight(left.GetByte(), right.GetByte());

			break;
		}
		MARMOT_HANDLER(LEFT_SHIFT_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftLeft(left.GetWord(), right.GetWord());

			break;
		}
		MARMOT_HANDLER(RIGHT_SHIFT_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::ShiftRight(left.GetWord(), right.GetWord());

			break;
		}
		MARMOT_HANDLER(BITWISE_AND)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() & right.GetInteger();

			break;
		}
		MARMOT_HANDLER(BITWISE_OR)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() | right.GetInteger();

			break;
		}
		MARMOT_HANDLER(BITWISE_XOR)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() ^ right.GetInteger();

			break;
		}
		MARMOT_HANDLER(BITWISE_NOT)
		{
			MarmotValue& right = Peek(sp);

			right = ~right.GetInteger();

			break;
		}
		MARMOT_HANDLER(ADD_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() + right.GetFloat();

			break;
		}
		MARMOT_HANDLER(SUBTRACT_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() - right.GetFloat();

			break;
		}
		MARMOT_HANDLER(MULTIPLY_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() * right.GetFloat();

			break;
		}
		MARMOT_HANDLER(DIVIDE_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() / right.GetFloat();

			break;
		}
		MARMOT_HANDLER(MODULO_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = std::fmod(left.GetFloat(), right.GetFloat());

			break;
		}
		MARMOT_HANDLER(ADD_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::Add(left.GetInteger(), right.GetInteger());

			break;
		}
		MARMOT_HANDLER(SUBTRACT_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::Subtract(left.GetInteger(), right.GetInteger());

			break;
		}
		MARMOT_HANDLER(MULTIPLY_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::Multiply(left.GetInteger(), right.GetInteger());

			break;
		}
		MARMOT_HANDLER(DIVIDE_INTEGER)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::Divide(left.GetInteger(), right.GetInteger());

			break;
		}
		MARMOT_HANDLER(MODULO_INTEGER)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = MarmotIntegerArithmetic::Remainder(left.GetInteger(), right.GetInteger());

			break;
		}
		MARMOT_HANDLER(ADD_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = static_cast<MarmotByte>(left.GetByte() + right.GetByte());

			break;
		}
		MARMOT_HANDLER(SUBTRACT_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = static_cast<MarmotByte>(left.GetByte() - right.GetByte());

			break;
		}
		MARMOT_HANDLER(MULTIPLY_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = static_cast<MarmotByte>(left.GetByte() * right.GetByte());

			break;
		}
		MARMOT_HANDLER(DIVIDE_BYTE)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = static_cast<MarmotByte>(left.GetByte() / right.GetByte());

			break;
		}
		MARMOT_HANDLER(MODULO_BYTE)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = static_cast<MarmotByte>(left.GetByte() % right.GetByte());

			break;
		}
		MARMOT_HANDLER(ADD_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() + right.GetWord();

			break;
		}
		MARMOT_HANDLER(SUBTRACT_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() - right.GetWord();

			break;
		}
		MARMOT_HANDLER(MULTIPLY_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() * right.GetWord();

			break;
		}
		MARMOT_HANDLER(DIVIDE_WORD)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() / right.GetWord();

			break;
		}
		MARMOT_HANDLER(MODULO_WORD)
		{
			m_instruction_pointer = inst_ip;
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() % right.GetWord();

			break;
		}
		MARMOT_HANDLER(CONCAT_ARRAY)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			MarmotArray& left_value_vector_ref = left.GetPointer()->GetTraceable<MarmotArray>();
			MarmotArray& right_value_vector_ref = right.GetPointer()->GetTraceable<MarmotArray>();
			MarmotArray result = MarmotArray::Concatenate(left_value_vector_ref, right_value_vector_ref);

			left = AllocateArray(std::move(result), left.GetPointer()->HoldsPointers());
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MARMOT_HANDLER(CONCAT_TEXT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			MarmotText& left_value_string_ref = left.GetPointer()->GetTraceable<MarmotText>();
			MarmotText& right_value_string_ref = right.GetPointer()->GetTraceable<MarmotText>();

			MarmotText result = MarmotText::Concatenate(left_value_string_ref, right_value_string_ref);

			left = AllocateTraceable(std::move(result));
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MARMOT_HANDLER(EXTEND_ARRAY)
		{
			MarmotTraceable* right = Pop(sp).GetPointer();
			MarmotTraceable* left = Peek(sp).GetPointer();
			const bool holds_pointers = left->HoldsPointers();

#if MARMOT_DEBUG_FULL
			if (holds_pointers != right->HoldsPointers())
			{
				std::println(stderr, "internal error: an array of {} is extended with one of {}.", holds_pointers ? "pointers" : "scalars", right->HoldsPointers() ? "pointers" : "scalars");
				std::abort();
			}
#endif
			if (holds_pointers)
			{
				m_gc.WriteBarrier(left);
			}
			left->GetTraceable<MarmotArray>().Extend(right->GetTraceable<MarmotArray>());

			break;
		}
		MARMOT_HANDLER(EXTEND_TEXT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left.GetPointer()->GetTraceable<MarmotText>().Append(right.GetPointer()->GetTraceable<MarmotText>());

			break;
		}
		MARMOT_HANDLER(ADD_LOCAL_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MarmotInteger imm = static_cast<MarmotInteger>(static_cast<int8_t>(ReadByte(ip)));

			MarmotValue& slot = *(bp + local_index);
			MarmotInteger result = MarmotIntegerArithmetic::Add(slot.GetInteger(), imm);
			slot = result;
			Push(sp, result);
			break;
		}
		MARMOT_HANDLER(PUSH_LOCAL_SUB_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MarmotInteger imm = static_cast<MarmotInteger>(static_cast<int8_t>(ReadByte(ip)));

			Push(sp, MarmotIntegerArithmetic::Subtract((bp + local_index)->GetInteger(), imm));
			break;
		}
		MARMOT_HANDLER(IF_LOCAL_LE_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MarmotInteger imm = static_cast<MarmotInteger>(static_cast<int8_t>(ReadByte(ip)));
			int offset = ReadShort(ip);

			if (!((bp + local_index)->GetInteger() <= imm))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_LOCAL_GE_LOCAL)
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
		MARMOT_HANDLER(GET_LOCAL2)
		{
			int first_index = static_cast<int>(ReadByte(ip));
			int second_index = static_cast<int>(ReadByte(ip));

			Push(sp, *(bp + first_index));
			Push(sp, *(bp + second_index));
			break;
		}
		MARMOT_HANDLER(STORE_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			*(bp + offset) = Pop(sp);
			break;
		}
		MARMOT_HANDLER(IF_LOCAL_TAG_NOT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			int tag = static_cast<int>(ReadByte(ip));
			int offset = ReadShort(ip);

			if ((bp + local_index)->GetPointer()->GetIndex() != tag)
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(LOCAL_UNION_FIELD)
		{
			int union_index = static_cast<int>(ReadByte(ip));
			int field_index = static_cast<int>(ReadByte(ip));
			int target_index = static_cast<int>(ReadByte(ip));

			*(bp + target_index) = (bp + union_index)->GetPointer()->GetValues()[static_cast<size_t>(field_index)];
			break;
		}
		MARMOT_HANDLER(IF_LOCAL_LT_INT)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MarmotInteger imm = static_cast<MarmotInteger>(static_cast<int8_t>(ReadByte(ip)));
			int offset = ReadShort(ip);

			if (!((bp + local_index)->GetInteger() < imm))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_LOCAL_LT_LOCAL)
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
		MARMOT_HANDLER(IF_LOCAL_EQ_LOCAL)
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
		MARMOT_HANDLER(STEP_LOCAL)
		{
			int local_index = static_cast<int>(ReadByte(ip));
			MarmotInteger imm = static_cast<MarmotInteger>(static_cast<int8_t>(ReadByte(ip)));

			MarmotValue& slot = *(bp + local_index);
			slot = MarmotIntegerArithmetic::Add(slot.GetInteger(), imm);
			break;
		}
		MARMOT_HANDLER(LOCAL_ARRAY_GET)
		{
			MarmotArray& arr_ref = (bp + static_cast<int>(ReadByte(ip)))->GetPointer()->GetTraceable<MarmotArray>();
			const MarmotInteger index = (bp + static_cast<int>(ReadByte(ip)))->GetInteger();
			int target_index = static_cast<int>(ReadByte(ip));

			if (IsIndexOutOfBounds(index, arr_ref.GetLength())) [[unlikely]]
			{
				return IndexOutOfBounds(index, inst_ip, sp, bp, env, closure);
			}

			*(bp + target_index) = arr_ref[static_cast<int>(index)];
			break;
		}
		MARMOT_HANDLER(LOCAL_UNION2)
		{
			int tag = static_cast<int>(ReadByte(ip));
			const std::array<MarmotValue, 2uz> fields{ *(bp + static_cast<int>(ReadByte(ip))), *(bp + static_cast<int>(ReadByte(ip))) };
			int target_index = static_cast<int>(ReadByte(ip));
			const uint16_t shape = static_cast<uint16_t>(ReadShort(ip));

			*(bp + target_index) = AllocateAggregate(MarmotTraceable::TraceableType::Union, std::span<const MarmotValue>(fields), tag, shape);
			break;
		}
		MARMOT_HANDLER(APPEND_LOCAL)
		{
			MarmotTraceable* arr = (bp + static_cast<int>(ReadByte(ip)))->GetPointer();
			MarmotValue val = *(bp + static_cast<int>(ReadByte(ip)));
			const bool holds_pointers = arr->HoldsPointers();

#if MARMOT_DEBUG_FULL
			CheckKind(val, holds_pointers, "an array appended to");
#endif
			if (holds_pointers)
			{
				m_gc.WriteBarrier(arr);
			}
			arr->GetTraceable<MarmotArray>().AddBack(val);
			break;
		}
		MARMOT_HANDLER(EQUAL_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() == right.GetFloat();

			break;
		}
		MARMOT_HANDLER(NOT_EQUAL_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() != right.GetFloat();

			break;
		}
		MARMOT_HANDLER(GREATER_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() > right.GetFloat();

			break;
		}
		MARMOT_HANDLER(GREATER_EQUAL_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() >= right.GetFloat();

			break;
		}
		MARMOT_HANDLER(LESS_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() < right.GetFloat();

			break;
		}
		MARMOT_HANDLER(LESS_EQUAL_FLOAT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetFloat() <= right.GetFloat();

			break;
		}
		MARMOT_HANDLER(EQUAL_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() == right.GetInteger();

			break;
		}
		MARMOT_HANDLER(NOT_EQUAL_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() != right.GetInteger();

			break;
		}
		MARMOT_HANDLER(GREATER_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() > right.GetInteger();

			break;
		}
		MARMOT_HANDLER(GREATER_EQUAL_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() >= right.GetInteger();

			break;
		}
		MARMOT_HANDLER(LESS_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() < right.GetInteger();

			break;
		}
		MARMOT_HANDLER(LESS_EQUAL_INTEGER)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetInteger() <= right.GetInteger();

			break;
		}
		MARMOT_HANDLER(EQUAL_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() == right.GetByte();

			break;
		}
		MARMOT_HANDLER(NOT_EQUAL_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() != right.GetByte();

			break;
		}
		MARMOT_HANDLER(GREATER_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() > right.GetByte();

			break;
		}
		MARMOT_HANDLER(GREATER_EQUAL_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() >= right.GetByte();

			break;
		}
		MARMOT_HANDLER(LESS_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() < right.GetByte();

			break;
		}
		MARMOT_HANDLER(LESS_EQUAL_BYTE)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetByte() <= right.GetByte();

			break;
		}
		MARMOT_HANDLER(EQUAL_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() == right.GetWord();

			break;
		}
		MARMOT_HANDLER(NOT_EQUAL_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() != right.GetWord();

			break;
		}
		MARMOT_HANDLER(GREATER_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() > right.GetWord();

			break;
		}
		MARMOT_HANDLER(GREATER_EQUAL_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() >= right.GetWord();

			break;
		}
		MARMOT_HANDLER(LESS_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() < right.GetWord();

			break;
		}
		MARMOT_HANDLER(LESS_EQUAL_WORD)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetWord() <= right.GetWord();

			break;
		}
		MARMOT_HANDLER(EQUAL_TEXT)
		{
			MarmotValue right = Pop(sp);
			MarmotValue& left = Peek(sp);

			left = left.GetPointer()->GetTraceable<MarmotText>() == right.GetPointer()->GetTraceable<MarmotText>();

			break;
		}
		MARMOT_HANDLER(NOT)
		{
			MarmotValue& value = Peek(sp);
			value = !value.GetBool();
			break;
		}
		MARMOT_HANDLER(NEGATE_FLOAT)
		{
			MarmotValue& value = Peek(sp);
			value = -value.GetFloat();
			break;
		}
		MARMOT_HANDLER(NEGATE_INTEGER)
		{
			MarmotValue& value = Peek(sp);
			value = MarmotIntegerArithmetic::Negate(value.GetInteger());
			break;
		}
		MARMOT_HANDLER(JUMP_IF_FALSE)
		{
			MarmotValue value = Peek(sp);

			int offset = ReadShort(ip);
			if (!value.GetBool())
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(JUMP)
		{
			int offset = ReadShort(ip);
			ip += offset;
			break;
		}
		MARMOT_HANDLER(JUMP_BACK)
		{
			int offset = ReadShort(ip);
			ip -= offset;
			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}
			TryCollect(ip, sp, bp, env, closure);
			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_LESS)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left < right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_LESS_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left <= right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_GREATER)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left > right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_GREATER_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left >= right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left == right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_INTEGER_NOT_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotInteger right = Pop(sp).GetInteger();
			MarmotInteger left = Pop(sp).GetInteger();

			if (!(left != right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_LESS)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left < right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_LESS_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left <= right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_GREATER)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left > right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_GREATER_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left >= right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left == right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(IF_FLOAT_NOT_EQUAL)
		{
			int offset = ReadShort(ip);
			MarmotFloat right = Pop(sp).GetFloat();
			MarmotFloat left = Pop(sp).GetFloat();

			if (!(left != right))
			{
				ip += offset;
			}
			break;
		}
		MARMOT_HANDLER(GET_TAG)
		{
			MarmotValue union_val = Pop(sp);
			Push(sp, static_cast<MarmotInteger>(union_val.GetPointer()->GetIndex()));
			break;
		}
		MARMOT_HANDLER(CALL_FOREIGN)
		{
			MarmotValue foreign_function_name = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));
			uint8_t return_type = static_cast<uint8_t>(ReadByte(ip));
			uint8_t argument_kinds = static_cast<uint8_t>(ReadByte(ip));

#if MARMOT_DEBUG_FULL
			if (!foreign_function_name.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalFFITypeError, std::format("Type error: expected function name (Text), but got {}.", foreign_function_name.ToText().View()), GetLine()));
			}
#endif

			const std::string_view name = foreign_function_name.GetPointer()->GetTraceable<MarmotText>().View();
			const VmNativeFunction* function = m_dynamic_ffi_registry.FindFunction(name);
			if (function == nullptr)
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::FFIFunctionNotFound, std::format("Failed to load foreign function '{}'.", name), GetLine()));
			}

			constexpr std::string_view run_process_symbol = "marmotstdlib\x1f" "MARMOT_FFI_RunProcess";
			const bool offload_process = Cooperative && name == run_process_symbol;
			constexpr uint8_t process_argument_kinds = MmcWire::FFI_ARGUMENT_ARRAY_STRINGS
				| (MmcWire::FFI_ARGUMENT_C_STRING << MmcWire::FFI_ARGUMENT_BITS);
			if (offload_process && (arity != 2 || argument_kinds != process_argument_kinds ||
				(return_type & MmcWire::FFI_RETURN_KIND_MASK) != MmcWire::FFI_RETURN_ARRAY_STRINGS))
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalFFITypeError, "RunProcess requires an array of text arguments and a text directory, and returns an array of text.", GetLine()));
			}

			for (int i = arity - 1; i >= 0; i -= 1)
			{
				const int shift = MmcWire::FFI_ARGUMENT_BITS * i;
				const uint8_t kind = (argument_kinds >> shift) & MmcWire::FFI_ARGUMENT_MASK;
				if (!PassForeignArgument(kind, static_cast<size_t>(i), Pop(sp)))
				{
					SyncMachineState(ip, sp, bp, env, closure);
					return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InvalidConversion, std::format("Cannot pass foreign argument {} as a C string because it contains a NUL byte.", i + 1), GetLine()));
				}
			}
			if (offload_process)
			{
				m_pending_process = std::make_shared<BlockingProcessCall>
				(
					*function, *m_worker_registry->GetStdlibWorkerErrorHooks(),
					*static_cast<const MarmotFFIArray*>(m_ffi_args[0u]), static_cast<const char*>(m_ffi_args[1u])
				);
				m_suspension = m_worker_registry->GetScheduler()->StartProcess(m_scheduled_id, m_pending_process, m_stop_token);
				SyncMachineState(ip, sp, bp, env, closure);
				return 0;
			}
			MarmotWord result = 0;
			function->m_call(m_ffi_args.data(), &result);
			Push(sp, TakeNativeResult(return_type, result, function->m_free_result));

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		MARMOT_HANDLER(CALL_INTRINSIC)
		{
			const uint8_t intrinsic_id = static_cast<uint8_t>(ReadByte(ip));
			const bool holds_pointers = static_cast<uint8_t>(ReadByte(ip)) != 0u;
			if (m_is_worker && intrinsic_id == static_cast<uint8_t>(VmIntrinsics::Id::Exit))
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerExited, "Worker exited. Exiting the process from a worker would end the whole program, so the worker fails instead and the joiner receives Err(Failed(...)). A panic inside a worker takes this path.", GetLine()));
			}

			if constexpr (Cooperative)
			{
				if (intrinsic_id == static_cast<uint8_t>(VmIntrinsics::Id::Exit))
				{
					SyncMachineState(ip, sp, bp, env, closure);
					return static_cast<int>(sp[-1].GetInteger());
				}
				if (intrinsic_id == static_cast<uint8_t>(VmIntrinsics::Id::Sleep))
				{
					const std::chrono::milliseconds delay(sp[-1].GetInteger());
					if (delay.count() > 0)
					{
						using Clock = CooperativeScheduler::Clock;
						const Clock::time_point now = Clock::now();
						const Clock::time_point latest = (Clock::time_point::max)();
						const std::chrono::milliseconds available = std::chrono::duration_cast<std::chrono::milliseconds>(latest - now);
						const Clock::time_point deadline = delay > available ? latest : now + delay;
						m_worker_registry->GetScheduler()->Sleep(m_scheduled_id, deadline, m_stop_token);
						m_suspension = deadline;
						sp[-1] = MarmotValue();
						SyncMachineState(ip, sp, bp, env, closure);
						return 0;
					}
				}
			}
			const VmIntrinsics::Intrinsic& intrinsic = VmIntrinsics::TABLE[intrinsic_id];
			sp -= intrinsic.m_arity;
			const MarmotValue result = intrinsic.m_call(*this, std::span<const MarmotValue>(sp, intrinsic.m_arity), holds_pointers);
			Push(sp, result);

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}

			break;
		}
		MARMOT_HANDLER(CALL)
		{
			MarmotValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MARMOT_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			env = closure->GetValues();

			ip = proc_entries[static_cast<size_t>(closure->GetIndex())];
			bp = sp - arity;

			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(CALL_0)
		MARMOT_HANDLER(CALL_1)
		MARMOT_HANDLER(CALL_2)
		MARMOT_HANDLER(CALL_3)
		{
			MarmotValue callable = Pop(sp);
			int arity = static_cast<int>(instruction) - static_cast<int>(VmOpCode::CALL_0);

#if MARMOT_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Save caller's frame before switching to callee
			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			env = closure->GetValues();

			ip = proc_entries[static_cast<size_t>(closure->GetIndex())];
			bp = sp - arity;

			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(CALL_PROC_WIDE)
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			proc_index |= static_cast<int>(ReadByte(ip)) << 8;
			int arity = static_cast<int>(ReadByte(ip));

			PushCallFrame(csp, bp, ip, env, closure);

			env = nullptr;
			closure = nullptr;
			ip = proc_entries[static_cast<size_t>(proc_index)];
			bp = sp - arity;

			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(CALL_GLOBAL_WIDE)
		{
			const int global_idx = ReadShort(ip);
			int arity = static_cast<int>(ReadByte(ip));
			MarmotValue callable = (*m_global_vars)[global_idx];

#if MARMOT_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			PushCallFrame(csp, bp, ip, env, closure);

			closure = callable.GetPointer();
			env = closure->GetValues();

			ip = proc_entries[static_cast<size_t>(closure->GetIndex())];
			bp = sp - arity;

			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(TAIL_CALL)
		{
			MarmotValue callable = Pop(sp);
			int arity = static_cast<int>(ReadByte(ip));

#if MARMOT_DEBUG_FULL
			if (!callable.IsPointer())
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, std::format("Type error: expected callable (function/closure), but got {}.", callable.ToText().View()), GetLine()));
			}
#endif

			// Move arguments down to base pointer
			MarmotValue* args_source = sp - arity;
			if (arity > 0 && args_source != bp)
			{
				std::memmove(bp, args_source, arity * sizeof(MarmotValue));
			}
			sp = bp + arity;

			closure = callable.GetPointer();
			env = closure->GetValues();

			// Jump to the start of the function without creating a new call frame
			ip = proc_entries[static_cast<size_t>(closure->GetIndex())];

			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}
			// Iteration is recursion: a tail call through a function value is a
			// loop the compiler cannot turn into JUMP_BACK, and without a
			// safepoint here it never collects.
			TryCollect(ip, sp, bp, env, closure);
			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}

			break;
		}
		MARMOT_HANDLER(CONSTRUCT_STRUCT)
		{
			int size = static_cast<int>(ReadByte(ip));
			const uint16_t shape = static_cast<uint16_t>(ReadShort(ip));
			sp -= size;
			Push(sp, AllocateAggregate(MarmotTraceable::TraceableType::Struct, std::span<const MarmotValue>(sp, static_cast<size_t>(size)), 0, shape));
			break;
		}
		MARMOT_HANDLER(CONSTRUCT_UNION)
		{
			int size = static_cast<int>(ReadByte(ip));
			int tag = static_cast<int>(ReadByte(ip));
			const uint16_t shape = static_cast<uint16_t>(ReadShort(ip));
			sp -= size;
			Push(sp, AllocateAggregate(MarmotTraceable::TraceableType::Union, std::span<const MarmotValue>(sp, static_cast<size_t>(size)), tag, shape));
			break;
		}
		MARMOT_HANDLER(LOAD_EMPTY_UNION)
		{
			Push(sp, EmptyUnion(static_cast<int>(ReadByte(ip))));
			break;
		}
		MARMOT_HANDLER(MAKE_CLOSURE_OF)
		{
			int proc_index = static_cast<int>(ReadByte(ip));
			proc_index |= static_cast<int>(ReadByte(ip)) << 8;
			int count = static_cast<int>(ReadByte(ip));

			// A capture is never written after SET_CAPTURE fills it, so the closure
			// holds the values themselves rather than a cell for each.
			ValueStackPointer captures = sp - count;
			MarmotTraceable* made = AllocateClosure(proc_index, std::span<const MarmotValue>(captures, static_cast<size_t>(count)));
			sp = captures;
			Push(sp, made);
			break;
		}
		MARMOT_HANDLER(SET_CAPTURE)
		{
			int index = static_cast<int>(ReadByte(ip));
			MarmotValue value = Pop(sp);
			MarmotTraceable* target = Pop(sp).GetPointer();
#if MARMOT_DEBUG_FULL
			CheckKind(value, std::ranges::binary_search(m_executable->GetShapes()[target->GetShape()], static_cast<uint32_t>(index)), "a capture bound late");
#endif
			m_gc.WriteBarrier(target);
			target->GetValues()[static_cast<size_t>(index)] = value;
			break;
		}
		MARMOT_HANDLER(GET_UNION_FIELD)
		{
			int index = static_cast<int>(ReadByte(ip));
			MarmotValue& top = Peek(sp);
			top = top.GetPointer()->GetValues()[static_cast<size_t>(index)];
			break;
		}
		MARMOT_HANDLER(MAKE_FUNCTION_WIDE)
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
				MarmotTraceable* new_closure = AllocateClosure(proc_index, {});
				if (cache_index < m_static_closure_cache.size())
				{
					m_static_closure_cache[cache_index] = new_closure;
				}
				Push(sp, new_closure);
			}

			break;
		}
		MARMOT_HANDLER(GET_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			Push(sp, *(bp + offset));
			break;
		}
		MARMOT_HANDLER(SET_LOCAL)
		{
			int offset = static_cast<int>(ReadByte(ip));
			*(bp + offset) = Peek(sp);
			break;
		}
		MARMOT_HANDLER(GET_CELL)
		{
			int offset = static_cast<int>(ReadByte(ip));
#if MARMOT_DEBUG_FULL
			if (!env)
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::InternalTypeError, "GET_CELL called with null environment - function has captures but was called via CALL_PROC_WIDE", GetLine()));
			}
#endif
			Push(sp, env[offset]);
			break;
		}
		MARMOT_HANDLER(MAKE_CELL)
		{
			// The initial value stays on the stack, and so stays rooted, until the
			// cell holding it replaces it.
			const bool holds_pointer = ReadByte(ip) != VmOpCode{ 0u };
			MarmotValue& top = Peek(sp);
			top = AllocateCell(top, holds_pointer);
			TryCollect(ip, sp, bp, env, closure);
			break;
		}
		MARMOT_HANDLER(READ_CELL)
		{
			MarmotValue& top = Peek(sp);
			top = top.GetPointer()->GetTraceable<MarmotMutableCell>().m_value;
			break;
		}
		MARMOT_HANDLER(WRITE_CELL)
		{
			MarmotValue value = Pop(sp);
			MarmotTraceable* cell = Pop(sp).GetPointer();
			const bool holds_pointers = cell->HoldsPointers();
#if MARMOT_DEBUG_FULL
			CheckKind(value, holds_pointers, "a Ref written");
#endif
			if (holds_pointers)
			{
				m_gc.WriteBarrier(cell);
			}
			cell->GetTraceable<MarmotMutableCell>().m_value = value;
			Push(sp, value);
			break;
		}
		MARMOT_HANDLER(DEFINE_GLOBAL_WIDE)
		{
			MarmotValue value = Pop(sp);
			const int global_idx = ReadShort(ip);
#if MARMOT_DEBUG_FULL
			CheckKind(value, m_executable->IsGlobalPointer(global_idx), m_executable->GetGlobalVariable(global_idx));
#endif
			MarmotValue& var = (*m_global_vars)[global_idx];
			var = value;
			break;
		}
		MARMOT_HANDLER(GET_GLOBAL_WIDE)
		{
			const int global_idx = ReadShort(ip);
			Push(sp, (*m_global_vars)[global_idx]);
			break;
		}
		MARMOT_HANDLER(SET_GLOBAL_WIDE)
		{
			const int global_idx = ReadShort(ip);
#if MARMOT_DEBUG_FULL
			CheckKind(Peek(sp), m_executable->IsGlobalPointer(global_idx), m_executable->GetGlobalVariable(global_idx));
#endif
			MarmotValue& var = (*m_global_vars)[global_idx];
			var = Peek(sp);
			break;
		}
		MARMOT_HANDLER(GET_LOCAL_WIDE)
		{
			const int offset = ReadShort(ip);
			Push(sp, *(bp + offset));
			break;
		}
		MARMOT_HANDLER(SET_LOCAL_WIDE)
		{
			const int offset = ReadShort(ip);
			*(bp + offset) = Peek(sp);
			break;
		}
		MARMOT_HANDLER(GET_CELL_WIDE)
		{
			const int offset = ReadShort(ip);
			Push(sp, env[offset]);
			break;
		}
		MARMOT_HANDLER(GET_MEMBER)
		{
			int index = static_cast<int>(ReadByte(ip));
			MarmotValue value = Pop(sp);
			Push(sp, value.GetPointer()->GetValues()[static_cast<size_t>(index)]);
			break;
		}
		MARMOT_HANDLER(POP)
		{
			--sp;
			break;
		}
		MARMOT_HANDLER(RETURN)
		{
			MarmotValue value = Pop(sp);
			--csp;
			m_call_stack_pointer = csp;
			const CallFrame& frame = *csp;

			// Callee's bp points to where args started, which is our return point
			ValueStackPointer return_point = bp;

			bp = frame.m_return_bp;
			sp = return_point;
			ip = frame.m_return_ip;
			env = frame.m_captures;
			closure = frame.m_closure;

			Push(sp, value);
			// Every call that is not a tail call returns, so recursion that never
			// loops (a tree walk) still reaches a safepoint, to be cancelled or to
			// collect what it allocated.
			if (IsCancellationRequested()) [[unlikely]]
			{
				SyncMachineState(ip, sp, bp, env, closure);
				return TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::WorkerCancelled, "Worker cancelled.", GetLine()));
			}
			TryCollect(ip, sp, bp, env, closure);

			if (YieldAfterCheckpoint<Cooperative>(remaining_checkpoints, ip, sp, bp, env, closure)) [[unlikely]]
			{
				return 0;
			}
			break;
		}
		MARMOT_HANDLER(HALT)
		{
			SyncMachineState(ip, sp, bp, env, closure);
			return 0;
		}
		MARMOT_HANDLER(PUSH_PLACEHOLDER)
		{
			int count = static_cast<int>(ReadByte(ip));
			sp = std::fill_n(sp, count, MarmotValue());
			break;
		}
		MARMOT_HANDLER(SPAWN_WORKER)
		MARMOT_HANDLER(JOIN_WORKER)
		MARMOT_HANDLER(CHANNEL_CREATE)
		MARMOT_HANDLER(CHANNEL_SEND)
		MARMOT_HANDLER(CHANNEL_RECEIVE)
		MARMOT_HANDLER(CHANNEL_CLOSE)
		MARMOT_HANDLER(WORKER_IS_DONE)
		MARMOT_HANDLER(WORKER_CANCEL)
		{
			SyncMachineState(ip, sp, bp, env, closure);
			// Through a copy: once ip's address escapes, every dispatch stores it.
			InstructionPointer operands = ip;
			if (!ExecuteConcurrencyInstruction(instruction, operands))
			{
				if constexpr (Cooperative)
				{
					if (m_suspension.has_value())
					{
						// The operands are still on the stack for the retry.
						m_instruction_pointer = inst_ip;
						return 0;
					}
				}
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

	return std::ranges::any_of
	(
		regions,
		[this, fault_address](const std::pair<void*, size_t>& region) -> bool
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
		}
	);
}
#endif

VirtualMachine::ExecuteResult VirtualMachine::ExecuteSlice() noexcept
{
	const StdlibWorkerErrorHooks* const hooks = m_worker_registry->GetStdlibWorkerErrorHooks();
	if (hooks == nullptr)
	{
		return Execute();
	}
	if (m_pending_process != nullptr)
	{
		m_stdlib_worker_errors = m_pending_process->TakeWorkerErrors();
	}
	void* const previous = hooks->m_exchange(m_stdlib_worker_errors.release());
	ExecuteResult result = Execute();
	m_stdlib_worker_errors = std::unique_ptr<void, StdlibWorkerErrorHooks::Free>(hooks->m_exchange(previous), hooks->m_free);
	return result;
}

VirtualMachine::ExecuteResult VirtualMachine::Execute() noexcept
{
	m_last_error.reset();
	m_suspension.reset();

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
	std::snprintf
	(
		message,
		sizeof(message),
		"Memory access violation - possible bytecode corruption or invalid operation (exception at %p, fault address %p).",
		reinterpret_cast<void*>(exception_address),
		reinterpret_cast<void*>(fault_address)
	);
	static_cast<void>(TerminateExecution(GenerateRuntimeError(RuntimeErrorCode::MemoryAccessViolation, message, GetLine())));
	return std::unexpected(std::move(*m_last_error));
#else
	UnixSignalInfo signal_info;
	sigjmp_buf jump_buffer;
	UnixSignalHandlerState handler_state;
	handler_state.m_jump_buffer = &jump_buffer;
	handler_state.m_signal_info = &signal_info;

	if (!InstallVirtualMachineSignalHandlers())
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
		RestoreVirtualMachineSignalHandlers();
		if (m_last_error.has_value())
		{
			return std::unexpected(std::move(*m_last_error));
		}
		return result;
	}

	s_active_unix_signal_handler = nullptr;
	RestoreVirtualMachineSignalHandlers();

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




