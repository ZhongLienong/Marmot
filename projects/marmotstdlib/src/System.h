#pragma once

#include "FFIAllocation/FFIAllocation.h"

#include <string>
#include <string_view>
#include <vector>

namespace MarmotSystem
{
	[[nodiscard]] FFIAllocation::FFIArray* RunProcess(
		const std::vector<std::string>& arguments,
		std::string_view directory
	) noexcept;
}
