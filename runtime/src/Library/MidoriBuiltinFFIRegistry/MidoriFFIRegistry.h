#pragma once

#include "Common/Builtins/BuiltinTable.h"
#include "Library/MidoriStdLibExports.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

struct FFIEntry
{
	const char* m_name;
	FFIFunction m_function;
	std::array<FFIArgumentKind, MIDORI_FFI_MAX_ARITY> m_arg_kinds{};
	FFIReturnKind m_return_kind = FFIReturnKind::RawValue;

	constexpr FFIEntry
	(
		const char* name,
		FFIFunction function,
		std::array<FFIArgumentKind, MIDORI_FFI_MAX_ARITY> arg_kinds = {},
		FFIReturnKind return_kind = FFIReturnKind::RawValue
	)
		: m_name(name), m_function(function), m_arg_kinds(arg_kinds), m_return_kind(return_kind)
	{
	}
};

// The runtime half of the builtin table: the same entries as MarmotBuiltins,
// expanded from the same Builtins.def, with the function each one calls.
class MidoriFFIRegistry
{
private:
	inline static constexpr std::array s_entries =
	{
#define MARMOT_BUILTIN(name, arg_kinds, return_kind) FFIEntry{ #name, &name, arg_kinds, return_kind },
#include "Common/Builtins/Builtins.def"
#undef MARMOT_BUILTIN
	};

public:
	static constexpr int ABI_VERSION = MarmotBuiltins::ABI_VERSION;
	static constexpr size_t BUILTIN_COUNT = s_entries.size();
	static_assert(BUILTIN_COUNT == MarmotBuiltins::COUNT, "the runtime and shared builtin tables must list the same entries");

	static consteval size_t ExitBuiltinIndex()
	{
		return MarmotBuiltins::ExitIndex();
	}

	static const FFIEntry& GetEntry(size_t index);
	static std::optional<size_t> FindIndex(std::string_view name);
	static constexpr size_t GetTableSize();
	static const std::array<FFIEntry, BUILTIN_COUNT>& GetTable();
};
