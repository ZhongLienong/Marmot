#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "Bytecode/Intrinsics/IntrinsicTable.h"
#include "Interpreter/VirtualMachine/VirtualMachine.h"
#include "Library/SharedLibraryCache/SharedLibraryCache.h"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
	constexpr VmOpCode zero{ 0u };
	constexpr VmOpCode first{ 1u };
	constexpr VmOpCode second{ 2u };
	constexpr VmOpCode sleep_id{ static_cast<uint8_t>(VmIntrinsics::Id::Sleep) };

	struct Rendezvous
	{
		std::mutex m_mutex;
		std::condition_variable m_ready;
		std::thread::id m_first;
		bool m_overlapped = false;
	};

	Rendezvous* s_rendezvous = nullptr;

	void MARMOT_VM_NATIVE_CALL MeetWorker(void**, void* result)
	{
		using namespace std::chrono_literals;
		Rendezvous& state = *s_rendezvous;
		std::unique_lock<std::mutex> lock(state.m_mutex);
		const std::thread::id current = std::this_thread::get_id();
		if (state.m_first == std::thread::id{})
		{
			state.m_first = current;
		}
		else if (current != state.m_first)
		{
			state.m_overlapped = true;
			state.m_ready.notify_all();
		}
		const MarmotWord overlapped = state.m_ready.wait_for(lock, 3s, [&]() { return state.m_overlapped; }) ? 1u : 0u;
		std::memcpy(result, &overlapped, sizeof(overlapped));
	}

	void MARMOT_VM_NATIVE_CALL FreeTestResult(uint8_t, void*)
	{
	}

	VmExecutable ChannelProgram(std::vector<VmOpCode> main, VmBytecodeStream worker)
	{
		VmExecutable executable;
		executable.AttachShapes({ {}, { 0u }, { 1u } });
		executable.AttachProcedureNames({ "main", "Worker" });
		executable.AttachProcedures({ VmBytecodeStream(std::move(main), {}, {}, {}), std::move(worker) });
		return executable;
	}
}

TEST_CASE("A VM can suspend at Join and resume on the scheduler thread", "[runtime][worker][scheduler]")
{
	constexpr VmOpCode zero{ 0u };
	constexpr VmOpCode first{ 1u };
	constexpr VmOpCode second{ 2u };
	VmExecutable executable;
	executable.AttachShapes({ {}, { 0u } });
	executable.AttachProcedureNames({ "main", "Parent", "Child" });
	executable.AttachProcedures
	({
		VmBytecodeStream
		(
			{
				VmOpCode::INT_10,
				VmOpCode::MAKE_FUNCTION_WIDE, first, zero,
				VmOpCode::SPAWN_WORKER, zero, first, zero, zero,
				VmOpCode::JOIN_WORKER, zero,
				VmOpCode::GET_UNION_FIELD, zero,
				VmOpCode::ADD_INTEGER,
				VmOpCode::HALT
			},
			{}, {}, {}
		),
		VmBytecodeStream
		(
			{
				VmOpCode::INT_2,
				VmOpCode::MAKE_FUNCTION_WIDE, second, zero,
				VmOpCode::SPAWN_WORKER, zero, first, zero, zero,
				VmOpCode::JOIN_WORKER, zero,
				VmOpCode::GET_UNION_FIELD, zero,
				VmOpCode::ADD_INTEGER,
				VmOpCode::RETURN
			},
			{}, {}, {}
		),
		VmBytecodeStream({ VmOpCode::INT_1, VmOpCode::RETURN }, {}, {}, {})
	});

	VirtualMachine vm(std::move(executable));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm);
	REQUIRE(result.has_value());
	CHECK(*result == 0);
	CHECK(vm.PeekValue().GetInteger() == 13);
}

TEST_CASE("Channel backpressure and timers let another VM run", "[runtime][worker][scheduler]")
{
	using enum VmOpCode;
	const size_t threads = GENERATE(1u, 2u, 3u);
	CAPTURE(threads);
	VirtualMachine vm(ChannelProgram
	(
		{
			INT_1, CHANNEL_CREATE,
			GET_LOCAL, zero, MAKE_FUNCTION_WIDE, first, zero, SPAWN_WORKER, first, second, zero, zero,
			INT_1, CALL_INTRINSIC, sleep_id, zero, POP,
			GET_LOCAL, zero, CHANNEL_RECEIVE, zero, GET_UNION_FIELD, zero,
			GET_LOCAL, zero, CHANNEL_RECEIVE, zero, GET_UNION_FIELD, zero, ADD_INTEGER,
			GET_LOCAL, first, JOIN_WORKER, zero, GET_UNION_FIELD, zero, ADD_INTEGER,
			HALT
		},
		VmBytecodeStream
		(
			{
				GET_LOCAL, zero, INT_2, CHANNEL_SEND, zero, POP,
				GET_LOCAL, zero, INT_5, CHANNEL_SEND, zero, POP,
				INT_1, RETURN
			},
			{}, VmFrameMap{ { false } }, {}
		)
	));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm, threads);
	REQUIRE(result.has_value());
	CHECK(vm.PeekValue().GetInteger() == 8);
}

TEST_CASE("Two scheduler threads can run separate VMs at the same time", "[runtime][worker][scheduler]")
{
	using enum VmOpCode;
	const std::string library = "marmot_worker_scheduler_test";
	SharedLibraryCache& cache = SharedLibraryCache::GetInstance();
	cache.RegisterHostLibrary(library, FreeTestResult, [](std::string_view symbol) -> VmNativeAbi::Function
	{
		return symbol == "MeetWorker" ? MeetWorker : nullptr;
	});
	const std::optional<std::expected<void, std::string>> loaded = cache.LoadHostLibrary(library, { "MeetWorker" });
	REQUIRE(loaded.has_value());
	REQUIRE(loaded->has_value());

	VmExecutable executable;
	executable.AttachNativeLibraries({ VmNativeLibraryImport{ library, { "MeetWorker" }, {}, { .m_thread_safe = true } } });
	executable.AddStringPool({ library + VM_NATIVE_SYMBOL_SEPARATOR + "MeetWorker" });
	executable.AttachShapes({ {}, { 0u } });
	executable.AttachProcedureNames({ "main", "Meet" });
	executable.AttachProcedures
	({
		VmBytecodeStream
		(
			{
				MAKE_FUNCTION_WIDE, first, zero, SPAWN_WORKER, zero, first, zero, zero,
				LOAD_STRING_WIDE, zero, zero, CALL_FOREIGN, zero, zero, zero,
				GET_LOCAL, zero, JOIN_WORKER, zero, GET_UNION_FIELD, zero, ADD_INTEGER,
				HALT
			},
			{ { 1, 25 } }, {}, {}
		),
		VmBytecodeStream
		(
			{ LOAD_STRING_WIDE, zero, zero, CALL_FOREIGN, zero, zero, zero, RETURN },
			{ { 1, 8 } }, {}, {}
		)
	});

	Rendezvous state;
	s_rendezvous = &state;
	VirtualMachine vm(std::move(executable));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm, 2u);
	s_rendezvous = nullptr;
	REQUIRE(result.has_value());
	CHECK(vm.PeekValue().GetInteger() == 2);
}

TEST_CASE("Channel close and cancellation wake the right receivers", "[runtime][worker][scheduler]")
{
	using enum VmOpCode;
	const bool close_channel = GENERATE(false, true);
	CAPTURE(close_channel);
	std::vector<VmOpCode> main
	{
		INT_1, CHANNEL_CREATE,
		GET_LOCAL, zero, MAKE_FUNCTION_WIDE, first, zero, SPAWN_WORKER, first, second, zero, first,
		GET_LOCAL, zero, MAKE_FUNCTION_WIDE, first, zero, SPAWN_WORKER, first, second, zero, first,
		INT_1, CALL_INTRINSIC, sleep_id, zero, POP
	};
	if (close_channel)
	{
		main.insert(main.end(), { GET_LOCAL, zero, CHANNEL_CLOSE, POP });
	}
	else
	{
		main.insert(main.end(), { GET_LOCAL, zero, INT_5, CHANNEL_SEND, zero, POP, GET_LOCAL, first, WORKER_CANCEL, POP });
	}
	main.insert(main.end(), { GET_LOCAL, second, JOIN_WORKER, first, GET_LOCAL, first, JOIN_WORKER, first, HALT });
	VirtualMachine vm(ChannelProgram
	(
		std::move(main),
		VmBytecodeStream({ GET_LOCAL, zero, CHANNEL_RECEIVE, zero, RETURN }, {}, VmFrameMap{ { false } }, {})
	));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm);
	REQUIRE(result.has_value());

	const MarmotTraceable* first_outcome = vm.PeekValue().GetPointer();
	const MarmotTraceable* second_outcome = vm.GetValueStackPointer()[-2].GetPointer();
	REQUIRE(second_outcome->GetIndex() == 0);
	const MarmotTraceable* second_option = second_outcome->GetValues()[0].GetPointer();
	if (close_channel)
	{
		REQUIRE(first_outcome->GetIndex() == 0);
		CHECK(first_outcome->GetValues()[0].GetPointer()->GetIndex() == 0);
		CHECK(second_option->GetIndex() == 0);
	}
	else
	{
		REQUIRE(first_outcome->GetIndex() == 1);
		CHECK(first_outcome->GetValues()[0].GetPointer()->GetIndex() == 0);
		REQUIRE(second_option->GetIndex() == 1);
		CHECK(second_option->GetValues()[0].GetInteger() == 5);
	}
}

TEST_CASE("Cancelling a parked worker removes its timer", "[runtime][worker][scheduler]")
{
	using enum VmOpCode;
	VirtualMachine vm(ChannelProgram
	(
		{
			MAKE_FUNCTION_WIDE, first, zero, SPAWN_WORKER, zero, first, zero, zero,
			INT_1, CALL_INTRINSIC, sleep_id, zero, POP,
			GET_LOCAL, zero, WORKER_CANCEL, POP,
			JOIN_WORKER, zero, HALT
		},
		VmBytecodeStream
		(
			{
				INTEGER_CONSTANT, VmOpCode{ 96u }, VmOpCode{ 234u }, zero, zero, zero, zero, zero, zero,
				CALL_INTRINSIC, sleep_id, zero, POP, INT_1, RETURN
			},
			{}, {}, {}
		)
	));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm);
	REQUIRE(result.has_value());
	const MarmotTraceable* outcome = vm.PeekValue().GetPointer();
	REQUIRE(outcome->GetIndex() == 1);
	CHECK(outcome->GetValues()[0].GetPointer()->GetIndex() == 0);
}

TEST_CASE("The cooperative scheduler reports an unwakeable channel wait", "[runtime][worker][scheduler]")
{
	using enum VmOpCode;
	VmExecutable executable;
	executable.SetFileName("ChannelDeadlock.mmt");
	executable.AttachProcedureNames({ "main" });
	executable.AttachProcedures({ VmBytecodeStream({ INT_1, CHANNEL_CREATE, CHANNEL_RECEIVE, zero, HALT }, { { 1, 2 }, { 2, 3 } }, {}, {}) });

	VirtualMachine vm(std::move(executable));
	const VirtualMachine::ExecuteResult result = WorkerRegistry::RunCooperatively(vm);
	REQUIRE_FALSE(result.has_value());
	CHECK(result.error().m_code == RuntimeErrorCode::ConcurrencyDeadlock);
	REQUIRE(result.error().m_location.has_value());
	CHECK(result.error().m_location->m_line == 2);
}
