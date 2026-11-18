#pragma once

#include <string>
#include <string_view>
#include <vector>

// What a builtin hands the VM, which frees it: a C string, or an array of them.
namespace FFIAllocation
{
	struct FFIArray
	{
		void* data;
		int length;
	};

	[[nodiscard]] char* AllocateString(std::string_view text);

	// The VM reads no array as an empty one.
	[[nodiscard]] FFIArray* AllocateStringArray(const std::vector<std::string>& values);
}
