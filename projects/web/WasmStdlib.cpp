#include "Library/SharedLibraryCache/SharedLibraryCache.h"

MarmotFFIFn MarmotStdLibResolve(std::string_view symbol);

namespace
{
	const bool s_registered = []
	{
		SharedLibraryCache::GetInstance().RegisterHostLibrary(
			"marmotstdlib", &MARMOT_FFI_FreeResult, &MarmotStdLibResolve);
		return true;
	}();
}
