#include "FFI/MarmotVMFFIExports.h"

#include "Support/Cancellation/Cancellation.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

extern "C"
{
	void MARMOT_FFI_FUNC(Exit)(void** args, void*) noexcept
	{
		int64_t exit_code = 0;
		std::memcpy(&exit_code, &args[0u], sizeof(int64_t));
		std::exit(static_cast<int>(exit_code));
	}

	void MARMOT_FFI_FUNC(Sleep)(void** args, void* ret) noexcept
	{
		int64_t milliseconds = 0;
		std::memcpy(&milliseconds, &args[0u], sizeof(int64_t));
		ThreadCancellation::SleepInterruptible(std::chrono::milliseconds(milliseconds));
		std::memset(ret, 0, sizeof(double));
	}
}
