#pragma once

#include <string>
#include <string_view>

namespace MidoriAnalysis
{
	std::string DemangleDisplayName(std::string_view qualified_name);

	bool IsIgnoredBindingName(std::string_view name);
}
