#include "FFI/MarmotVMFFIExports.h"

#include "FFIAllocation/FFIAllocation.h"
#include "Library/ProgramArguments/ProgramArguments.h"
#include "Support/Cancellation/Cancellation.h"
#include "System.h"
#include "Value/Value.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C"
{
	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(Exit)(void** args, void*) noexcept
	{
		int64_t exit_code = 0;
		std::memcpy(&exit_code, &args[0u], sizeof(int64_t));
		std::exit(static_cast<int>(exit_code));
	}

	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(Sleep)(void** args, void* ret) noexcept
	{
		int64_t milliseconds = 0;
		std::memcpy(&milliseconds, &args[0u], sizeof(int64_t));
		ThreadCancellation::SleepInterruptible(std::chrono::milliseconds(milliseconds));
		std::memset(ret, 0, sizeof(double));
	}

	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(RunProcess)(void** args, void* ret) noexcept
	{
		const MarmotArray& values = static_cast<MarmotTraceable*>(args[0u])->GetTraceable<MarmotArray>();
		std::vector<std::string> arguments;
		arguments.reserve(static_cast<size_t>(values.GetLength()));
		for (int index = 0; index < values.GetLength(); index += 1)
		{
			arguments.emplace_back(values[index].GetPointer()->GetTraceable<MarmotText>().View());
		}

		const char* directory = reinterpret_cast<const char*>(args[1u]);
		FFIAllocation::FFIArray* result = MarmotSystem::RunProcess(arguments, directory);
		const int64_t pointer = reinterpret_cast<int64_t>(result);
		std::memcpy(ret, &pointer, sizeof(pointer));
	}

	MARMOT_VM_FFI_API void MARMOT_FFI_FUNC(GetArguments)(void**, void* ret) noexcept
	{
		const int64_t pointer = reinterpret_cast<int64_t>(
			FFIAllocation::AllocateStringArray(ProgramArguments::Get()));
		std::memcpy(ret, &pointer, sizeof(pointer));
	}
}
