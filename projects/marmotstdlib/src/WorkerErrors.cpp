#include "WorkerErrors.h"

#include "MarmotNativeAbi.h"

#include <memory>

namespace
{
	thread_local std::unique_ptr<StdlibWorkerErrors::State> s_current_errors;
}

StdlibWorkerErrors::State& StdlibWorkerErrors::Current()
{
	if (s_current_errors == nullptr)
	{
		s_current_errors = std::make_unique<State>();
	}
	return *s_current_errors;
}

// The VM swaps opaque states at slice boundaries. Allocation and release both
// stay in this library, including when it uses a separate C runtime.
extern "C" MARMOT_FFI_EXPORT void* MARMOT_FFI_CALL MARMOT_STDLIB_ExchangeWorkerErrors(void* state) noexcept
{
	StdlibWorkerErrors::State* previous = s_current_errors.release();
	s_current_errors.reset(static_cast<StdlibWorkerErrors::State*>(state));
	return previous;
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_STDLIB_FreeWorkerErrors(void* state) noexcept
{
	delete static_cast<StdlibWorkerErrors::State*>(state);
}
