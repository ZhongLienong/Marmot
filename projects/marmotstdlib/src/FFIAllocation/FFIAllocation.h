#pragma once

#include "MarmotNativeAbi.h"

#include <string>
#include <string_view>
#include <vector>

// What the library returns: a C string, or an array of them.
namespace FFIAllocation
{
	[[nodiscard]] char* AllocateString(std::string_view text);

	// The VM reads no array as an empty one.
	[[nodiscard]] MarmotFFIArray* AllocateStringArray(const std::vector<std::string>& values);
}
