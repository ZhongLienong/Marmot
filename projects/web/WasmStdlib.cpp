#include "Library/SharedLibraryCache/SharedLibraryCache.h"

extern "C" void MARMOT_VM_NATIVE_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr);
VmNativeAbi::Function MarmotStdLibResolve(std::string_view symbol);

namespace
{
	const bool s_registered = []
	{
		SharedLibraryCache::GetInstance().RegisterHostLibrary(
			"marmotstdlib", &MARMOT_FFI_FreeResult, &MarmotStdLibResolve);
		return true;
	}();
}
