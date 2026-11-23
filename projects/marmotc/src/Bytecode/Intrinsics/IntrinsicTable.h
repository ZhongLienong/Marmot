#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

struct MarmotIntrinsicSignature
{
	std::string_view m_name;
	std::span<const std::string_view> m_parameters;
	std::string_view m_result;
};

class MarmotIntrinsics
{
private:
#define MARMOT_INTRINSIC(name, result, ...) \
	inline static constexpr auto s_types_##name = std::to_array<std::string_view>({result, __VA_ARGS__});
#include "Bytecode/Intrinsics/Intrinsics.def"
#undef MARMOT_INTRINSIC

	inline static constexpr std::array s_signatures =
	{
#define MARMOT_INTRINSIC(name, result, ...) \
		MarmotIntrinsicSignature{ "MARMOT_FFI_" #name, std::span(s_types_##name).subspan(1u), result },
#include "Bytecode/Intrinsics/Intrinsics.def"
#undef MARMOT_INTRINSIC
	};

public:
	static constexpr std::optional<size_t> FindIndex(std::string_view name)
	{
		for (size_t index = 0uz; index < s_signatures.size(); index += 1uz)
		{
			if (s_signatures[index].m_name == name)
			{
				return index;
			}
		}
		return std::nullopt;
	}

	static constexpr const MarmotIntrinsicSignature& At(size_t index)
	{
		return s_signatures[index];
	}
};
