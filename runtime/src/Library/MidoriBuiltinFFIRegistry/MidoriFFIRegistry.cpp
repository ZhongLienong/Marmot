#include "MidoriFFIRegistry.h"

const FFIEntry& MidoriFFIRegistry::GetEntry(size_t index)
{
	return s_entries[index];
}

std::optional<size_t> MidoriFFIRegistry::FindIndex(std::string_view name)
{
	for (size_t i = 0uz; i < s_entries.size(); i += 1uz)
	{
		if (s_entries[i].m_name == name)
		{
			return i;
		}
	}
	return std::nullopt;
}

constexpr size_t MidoriFFIRegistry::GetTableSize()
{
	return s_entries.size();
}

const std::array<FFIEntry, MidoriFFIRegistry::BUILTIN_COUNT>& MidoriFFIRegistry::GetTable()
{
	return s_entries;
}
