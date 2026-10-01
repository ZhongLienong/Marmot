#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

// Only builtin names and indexes are needed to emit CALL_FOREIGN_INDEXED.

class MarmotBuiltins
{
private:
	inline static constexpr std::array s_names =
	{
#define MARMOT_BUILTIN(name) std::string_view(#name),
#include "Bytecode/Builtins/Builtins.def"
#undef MARMOT_BUILTIN
	};

public:
	static constexpr int ABI_VERSION = 1;
	static constexpr size_t COUNT = s_names.size();

	static constexpr std::optional<size_t> FindIndex(std::string_view name)
	{
		for (size_t index = 0uz; index < s_names.size(); index += 1uz)
		{
			if (s_names[index] == name)
			{
				return index;
			}
		}
		return std::nullopt;
	}

	static constexpr std::string_view At(size_t index)
	{
		return s_names[index];
	}
};
