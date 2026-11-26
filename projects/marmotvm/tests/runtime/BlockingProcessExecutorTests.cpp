#include <catch2/catch_test_macros.hpp>

#include "Interpreter/Worker/BlockingProcessCall.h"
#include "Interpreter/Worker/BlockingProcessExecutor.h"

#include <latch>
#include <condition_variable>
#include <mutex>
#include <set>

namespace
{
	const std::latch* s_release = nullptr;

	void MARMOT_FFI_CALL HoldProcess(void**, void*)
	{
		s_release->wait();
	}

	void* MARMOT_FFI_CALL ExchangeErrors(void*) noexcept
	{
		return nullptr;
	}

	void MARMOT_FFI_CALL FreeErrors(void*) noexcept
	{
	}
}

TEST_CASE("The process executor can cancel a queued call while all helpers are busy", "[runtime][worker][scheduler]")
{
	using namespace std::chrono_literals;
	std::latch release(1);
	s_release = &release;
	std::mutex mutex;
	std::condition_variable changed;
	std::set<MarmotInteger> completed;
	BlockingProcessExecutor executor([&](MarmotInteger id)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		completed.insert(id);
		changed.notify_one();
	});
	const VmNativeFunction function{ HoldProcess, nullptr, {} };
	const StdlibWorkerErrorHooks hooks{ ExchangeErrors, FreeErrors };
	const MarmotFFIArray arguments{ nullptr, 0 };
	for (MarmotInteger id = 1; id <= 5; ++id)
	{
		executor.Submit(id, std::make_shared<BlockingProcessCall>(function, hooks, arguments, ""));
	}
	const bool cancelled = executor.CancelQueued(5);
	release.count_down();
	REQUIRE(cancelled);

	std::unique_lock<std::mutex> lock(mutex);
	REQUIRE(changed.wait_for(lock, 5s, [&]() { return completed.size() == 4u; }));
	CHECK(completed == std::set<MarmotInteger>{ 1, 2, 3, 4 });
}
