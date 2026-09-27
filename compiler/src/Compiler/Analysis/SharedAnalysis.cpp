#include "SharedAnalysis.h"

#include "Common/Constant/Constant.h"

namespace MidoriAnalysis
{
	std::string DemangleDisplayName(std::string_view qualified_name)
	{
		const size_t separator_pos = qualified_name.rfind(NameSeparator);
		if (separator_pos == std::string_view::npos)
		{
			return std::string(qualified_name);
		}

		return std::string(qualified_name.substr(separator_pos + NameSeparator.length()));
	}

	bool IsIgnoredBindingName(std::string_view name)
	{
		return !name.empty() && name[0u] == '_';
	}
}
