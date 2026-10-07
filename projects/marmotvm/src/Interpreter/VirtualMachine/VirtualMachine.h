#pragma once

#include "Bytecode/Format/Format.h"
#include "Support/Attributes/Attributes.h"
#include "Error/RuntimeError.h"
#include "Bytecode/Executable/Executable.h"
#include "Interpreter/Allocator/MidoriAllocator.h"
#include "Interpreter/GarbageCollector/GarbageCollector.h"
#include "Library/MidoriBuiltinFFIRegistry/MidoriFFIRegistry.h"
#include "Library/DynamicFFIRegistry/DynamicFFIRegistry.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <stop_token>
#include <type_traits>
#include <vector>

class VirtualMachine
{
public:
    using GlobalVariables = std::vector<MidoriValue>;
	using ExecuteResult = std::expected<int, RuntimeError>;

	VirtualMachine(VmExecutable&& executable) noexcept;

	VirtualMachine(std::shared_ptr<const VmExecutable> shared_executable, int proc_index, const GlobalVariables* source_globals) noexcept;

    ~VirtualMachine();

    template<typename T>
        requires MidoriTraceablePayload<std::remove_cvref_t<T>>
    MIDORI_FORCE_INLINE MidoriTraceable* AllocateTraceable(T&& payload)
    {
        void* mem = m_allocator.Allocate(MidoriTraceable::HEADER_SIZE + sizeof(std::remove_cvref_t<T>));
        MidoriTraceable* traceable = new(mem) MidoriTraceable(std::forward<T>(payload));
        m_gc.RegisterObject(traceable);
        return traceable;
    }

    // A tuple, struct, union or closure; `index` is a union's tag or a closure's procedure.
    MIDORI_FORCE_INLINE MidoriTraceable* AllocateAggregate(MidoriTraceable::TraceableType type, std::span<const MidoriValue> values, int index) noexcept
    {
        void* mem = m_allocator.Allocate(MidoriTraceable::AggregateBytes(static_cast<int>(values.size())));
        MidoriTraceable* traceable = new(mem) MidoriTraceable(type, values, index);
        m_gc.RegisterObject(traceable);
        return traceable;
    }

private:
	using ValueStackPointer = MidoriValue*;
	using InstructionPointer = const VmOpCode*;

	struct CallFrame
	{
		ValueStackPointer m_return_bp;
		InstructionPointer m_return_ip;
		MidoriValue* m_captures;
		// The closure m_captures points into. A frame is often the only thing that
		// still holds its closure - `Make(x)(n)` calls one nothing else names - so the
		// collector roots it through here, or frees it under a running call.
		MidoriTraceable* m_closure;
	};
	using CallStackPointer = CallFrame*;

	// Not std::hardware_destructive_interference_size: GCC warns that its value
	// follows -mtune, which would let the layout of this header differ between
	// translation units.
	static constexpr size_t s_cache_line_size = 64uz;

    // Iteration is recursion, and anything not in tail position keeps a frame:
    // 10,000 values stopped plain recursion near 5,000 calls. The pages are
    // only touched as the stack grows; Windows reserves them and commits them
    // as the stack reaches them.
    static constexpr size_t s_value_stack_size = 1uz << 20;
    static constexpr size_t s_call_stack_size = 1uz << 19;
    static constexpr int s_max_stack_trace_depth = 20;

    struct FFIArrayArgument
    {
        void* data;
        int length;
    };

	// Hot Pointers (cache-line aligned for the dispatch loop)
	alignas(s_cache_line_size) InstructionPointer m_instruction_pointer = nullptr;
	ValueStackPointer m_value_stack_pointer = nullptr;
	ValueStackPointer m_value_stack_base_pointer = nullptr;
	ValueStackPointer m_value_stack_begin = nullptr;
	CallStackPointer m_call_stack_pointer = nullptr;
	CallStackPointer m_call_stack_begin = nullptr;
	MidoriTraceable* m_curr_closure_traceable = nullptr;
	MidoriValue* m_curr_environment = nullptr;

    // Warm VM State
	std::shared_ptr<const VmExecutable> m_owned_executable;
    const VmExecutable* m_executable = nullptr;
	GlobalVariables m_owned_globals;
    GlobalVariables* m_global_vars = nullptr;
    std::vector<InstructionPointer> m_proc_entry_cache;
    std::vector<MidoriTraceable*> m_string_literal_cache;

    // Allocation & GC
    MidoriAllocator m_allocator;
    GarbageCollector m_gc;
    GarbageCollector::GarbageCollectionRoots m_gc_roots_scratch;

    // FFI State
    DynamicFFIRegistry m_dynamic_ffi_registry;
    std::array<VmFFIFunction, MidoriFFIRegistry::BUILTIN_COUNT> m_ffi_table{};
    std::array<void*, UINT8_MAX> m_ffi_args{};
    std::array<MidoriValue, UINT8_MAX> m_ffi_value_args{};
    std::vector<FFIArrayArgument> m_ffi_array_args;
	bool m_ffi_table_initialized = false;

    // Cold Caches & Results
	std::vector<MidoriTraceable*> m_static_closure_cache;
	// A union variant with no fields carries nothing but its tag, and v2 values are
	// immutable, so every occurrence of one can be the same object.
	std::array<MidoriTraceable*, static_cast<size_t>(VM_MAX_UNION_TAG) + 1u> m_empty_union_cache{};
    std::unordered_map<std::string_view, MidoriTraceable*> m_small_string_pool;

    void* m_value_stack_region = nullptr;
    void* m_call_stack_region = nullptr;
    size_t m_value_stack_region_size = 0u;
    size_t m_call_stack_region_size = 0u;
    size_t m_stack_page_size = 0u;
	std::optional<RuntimeError> m_last_error = std::nullopt;
	int m_worker_proc_index = -1;
	// Set by PrepareWorkerCall, which only a worker goes through. A worker that
	// exits must fail itself rather than the process it shares with everyone else.
	bool m_is_worker = false;
	std::stop_token m_stop_token;
	bool m_stop_possible = false;


public:
    ExecuteResult Execute() noexcept;

    const GarbageCollector& GetGC() const noexcept { return m_gc; }

    MidoriValue* GetValueStackPointer() noexcept { return m_value_stack_pointer; }

    void AdvanceValueStackPointer() noexcept { ++m_value_stack_pointer; }

	MidoriValue PeekValue() const noexcept { return *(m_value_stack_pointer - 1); }

	MidoriValue MakeFunctionValue(int proc_index) noexcept;

	void SetGlobalValue(int global_index, MidoriValue value) noexcept { (*m_global_vars)[global_index] = value; }

    void PrepareWorkerCall(MidoriValue worker_function) noexcept;

    void SetStopToken(std::stop_token stop_token) noexcept
    {
        m_stop_token = std::move(stop_token);
        m_stop_possible = m_stop_token.stop_possible();
    }

private:
	MIDORI_FORCE_INLINE void SyncMachineState(InstructionPointer ip, ValueStackPointer sp, ValueStackPointer bp, MidoriValue* env, MidoriTraceable* closure) noexcept
	{
		m_instruction_pointer = ip;
		m_value_stack_pointer = sp;
		m_value_stack_base_pointer = bp;
		m_curr_environment = env;
		m_curr_closure_traceable = closure;
	}

	MIDORI_FORCE_INLINE bool IsCancellationRequested() const noexcept
	{
		return m_stop_possible && m_stop_token.stop_requested();
	}

	MIDORI_FORCE_INLINE void TryCollect(InstructionPointer ip, ValueStackPointer sp, ValueStackPointer bp, MidoriValue* env, MidoriTraceable* closure) noexcept
	{
		if (m_gc.ShouldCollect())
		{
			SyncMachineState(ip, sp, bp, env, closure);
			BuildGarbageCollectionRoots(m_gc_roots_scratch);
			m_gc.ReclaimMemory(m_gc_roots_scratch, m_allocator);
		}
	}

	bool ExecuteConcurrencyInstruction(VmOpCode instruction, InstructionPointer& ip) noexcept;

	int ExecuteLoop() noexcept;

	int TerminateExecution(RuntimeError error) noexcept;

#ifdef _WIN32
	int ExecuteLoopWithStructuredExceptionHandling(uintptr_t& exception_code, uintptr_t& exception_address, uintptr_t& fault_address, bool& captured) noexcept;

	bool CommitStackPages(uintptr_t fault_address) noexcept;
#endif

	int GetLine() noexcept;

	MIDORI_FORCE_INLINE VmOpCode ReadByte() noexcept
	{
		return *m_instruction_pointer++;
	}

	static MIDORI_FORCE_INLINE VmOpCode ReadByte(InstructionPointer& ip) noexcept
	{
		return *ip++;
	}

	MIDORI_FORCE_INLINE int ReadShort() noexcept
	{
		const uint8_t b0 = static_cast<uint8_t>(m_instruction_pointer[0u]);
		const uint8_t b1 = static_cast<uint8_t>(m_instruction_pointer[1u]);
		int value = static_cast<int>(static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8));
		m_instruction_pointer += 2;
		return value;
	}

	static MIDORI_FORCE_INLINE int ReadShort(InstructionPointer& ip) noexcept
	{
		const uint8_t b0 = static_cast<uint8_t>(ip[0u]);
		const uint8_t b1 = static_cast<uint8_t>(ip[1u]);
		int value = static_cast<int>(static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8));
		ip += 2;
		return value;
	}

	MIDORI_FORCE_INLINE int ReadThreeBytes() noexcept
	{
		const uint8_t b0 = static_cast<uint8_t>(m_instruction_pointer[0u]);
		const uint8_t b1 = static_cast<uint8_t>(m_instruction_pointer[1u]);
		const uint8_t b2 = static_cast<uint8_t>(m_instruction_pointer[2u]);
		int value = static_cast<int>(static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16));
		m_instruction_pointer += 3;
		return value;
	}

	static MIDORI_FORCE_INLINE int ReadThreeBytes(InstructionPointer& ip) noexcept
	{
		const uint8_t b0 = static_cast<uint8_t>(ip[0u]);
		const uint8_t b1 = static_cast<uint8_t>(ip[1u]);
		const uint8_t b2 = static_cast<uint8_t>(ip[2u]);
		int value = static_cast<int>(static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16));
		ip += 3;
		return value;
	}

	MIDORI_FORCE_INLINE MidoriInteger ReadIntegerConstant() noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, m_instruction_pointer, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		m_instruction_pointer += sizeof(MidoriInteger);
		return static_cast<MidoriInteger>(bits);
	}

	static MIDORI_FORCE_INLINE MidoriInteger ReadIntegerConstant(InstructionPointer& ip) noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, ip, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		ip += sizeof(MidoriInteger);
		return static_cast<MidoriInteger>(bits);
	}

	MIDORI_FORCE_INLINE MidoriFloat ReadFloatConstant() noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, m_instruction_pointer, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		m_instruction_pointer += sizeof(MidoriFloat);
		return std::bit_cast<MidoriFloat>(bits);
	}

	static MIDORI_FORCE_INLINE MidoriFloat ReadFloatConstant(InstructionPointer& ip) noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, ip, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		ip += sizeof(MidoriFloat);
		return std::bit_cast<MidoriFloat>(bits);
	}

	MIDORI_FORCE_INLINE MidoriByte ReadByteConstant() noexcept
	{
		MidoriByte value = static_cast<MidoriByte>(*m_instruction_pointer);
		m_instruction_pointer += sizeof(MidoriByte);
		return value;
	}

	static MIDORI_FORCE_INLINE MidoriByte ReadByteConstant(InstructionPointer& ip) noexcept
	{
		MidoriByte value = static_cast<MidoriByte>(*ip);
		ip += sizeof(MidoriByte);
		return value;
	}

	MIDORI_FORCE_INLINE MidoriWord ReadWordConstant() noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, m_instruction_pointer, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		m_instruction_pointer += sizeof(MidoriWord);
		return bits;
	}

	static MIDORI_FORCE_INLINE MidoriWord ReadWordConstant(InstructionPointer& ip) noexcept
	{
		uint64_t bits = 0u;
		std::memcpy(&bits, ip, sizeof(bits));
		if constexpr (std::endian::native == std::endian::big)
		{
			bits = std::byteswap(bits);
		}
		ip += sizeof(MidoriWord);
		return bits;
	}

	RuntimeError GenerateRuntimeError(RuntimeErrorCode code, std::string_view message, int line) noexcept;

	std::vector<RuntimeStackFrame> GenerateStackTrace() noexcept;

	int GetProcedureIndexFromIP(InstructionPointer ip) noexcept;

	int GetLineFromIP(InstructionPointer ip, int proc_index) noexcept;

	MIDORI_FORCE_INLINE InstructionPointer GetProcEntry(int proc_index) const noexcept
	{
		return m_proc_entry_cache[static_cast<size_t>(proc_index)];
	}

	MIDORI_FORCE_INLINE void PushCallFrame(ValueStackPointer m_return_bp, InstructionPointer m_return_ip, MidoriValue* m_captures, MidoriTraceable* m_closure) noexcept
	{
		*m_call_stack_pointer = CallFrame{m_return_bp, m_return_ip, m_captures, m_closure};
		++m_call_stack_pointer;
	}

	// The dispatch loop keeps the pointer in a register, but still stores it on
	// every push: a fault mid-loop (guard page, division by zero) builds its
	// stack trace from the member.
	MIDORI_FORCE_INLINE void PushCallFrame(CallStackPointer& csp, ValueStackPointer return_bp, InstructionPointer return_ip, MidoriValue* captures, MidoriTraceable* closure) noexcept
	{
		*csp = CallFrame{return_bp, return_ip, captures, closure};
		++csp;
		m_call_stack_pointer = csp;
	}

    MIDORI_FORCE_INLINE MidoriValue& Peek() noexcept
    {
        return *(m_value_stack_pointer - 1);
    }

    MIDORI_FORCE_INLINE MidoriValue Pop() noexcept
    {
        return *(--m_value_stack_pointer);
    }

    static MIDORI_FORCE_INLINE MidoriValue& Peek(ValueStackPointer sp) noexcept
    {
        return *(sp - 1);
    }

    static MIDORI_FORCE_INLINE MidoriValue Pop(ValueStackPointer& sp) noexcept
    {
        return *(--sp);
    }

    template<typename T>
        requires MidoriValueConstructible<T>
    static MIDORI_FORCE_INLINE void Push(ValueStackPointer& sp, T val) noexcept
    {
        *sp = val;
        ++sp;
    }

	// Inline, since the out-of-line call cost more than the comparison; only
	// the error is outlined.
	MIDORI_FORCE_INLINE int CheckIndexBounds(const MidoriValue index, MidoriInteger size) noexcept
	{
		const MidoriInteger at = index.GetInteger();
		if (at < 0ll || at >= size) [[unlikely]]
		{
			return IndexOutOfBounds(at);
		}
		return 0;
	}

	MIDORI_NOINLINE int IndexOutOfBounds(MidoriInteger index) noexcept;

    void BuildGarbageCollectionRoots(GarbageCollector::GarbageCollectionRoots& roots) const noexcept;

	void InitializeStacks() noexcept;

	void InitializeProcEntryCache() noexcept;

	bool IsStackGuardFault(uintptr_t fault_address) const noexcept;

	void LocateStackOverflowAtInnermostCall() noexcept;

	template<typename T>
        requires MidoriValueConstructible<T>
	MIDORI_FORCE_INLINE void Push(T val) noexcept
    {
        *m_value_stack_pointer = val;
        ++m_value_stack_pointer;
	}
};
