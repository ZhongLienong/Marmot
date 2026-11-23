#pragma once

#include "Value/Value.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

class VirtualMachine;

namespace VmIntrinsics
{
	enum class Id : uint8_t
	{
#define MARMOT_INTRINSIC(name, result, ...) name,
#include "Bytecode/Intrinsics/Intrinsics.def"
#undef MARMOT_INTRINSIC
	};

#define MARMOT_INTRINSIC(name, result, ...) MarmotValue name(VirtualMachine&, std::span<const MarmotValue>, bool) noexcept;
#include "Bytecode/Intrinsics/Intrinsics.def"
#undef MARMOT_INTRINSIC

	struct Intrinsic
	{
		std::string_view m_name;
		size_t m_arity;
		MarmotValue (*m_call)(VirtualMachine&, std::span<const MarmotValue>, bool) noexcept;
	};

	inline constexpr std::array TABLE =
	{
#define MARMOT_INTRINSIC(name, result, ...) \
		Intrinsic{ "MARMOT_FFI_" #name, std::initializer_list<std::string_view>{ __VA_ARGS__ }.size(), &name },
#include "Bytecode/Intrinsics/Intrinsics.def"
#undef MARMOT_INTRINSIC
	};
}
