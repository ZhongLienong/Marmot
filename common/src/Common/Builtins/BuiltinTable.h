#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The builtin foreign functions as the compiler and tools see them: names and
// marshalling kinds, in bytecode order. The function pointers live in the
// runtime's MidoriFFIRegistry, expanded from the same Builtins.def, so this
// header carries no runtime symbols.

using FFIFunction = void(*)(void** args, void* ret);

constexpr size_t MIDORI_FFI_MAX_ARITY = 4u;

enum class FFIArgumentKind : uint8_t
{
	RawValue = 0,
	CString,
	ArrayView,
	TraceableHandle,
	ValueHandle
};

enum class FFIReturnKind : uint8_t
{
	RawValue = 0,
	CString,
	ArrayValues,
	ArrayStrings,
	Value
};

template<typename... Kinds>
consteval std::array<FFIArgumentKind, MIDORI_FFI_MAX_ARITY> MakeFFIArgKinds(Kinds... kinds)
{
	static_assert(sizeof...(Kinds) <= MIDORI_FFI_MAX_ARITY);
	std::array<FFIArgumentKind, MIDORI_FFI_MAX_ARITY> result{};
	FFIArgumentKind values[] = { kinds... };
	for (size_t i = 0uz; i < sizeof...(Kinds); i += 1uz)
	{
		result[i] = values[i];
	}
	return result;
}

struct BuiltinSignature
{
	std::string_view m_name;
	std::array<FFIArgumentKind, MIDORI_FFI_MAX_ARITY> m_arg_kinds{};
	FFIReturnKind m_return_kind = FFIReturnKind::RawValue;
};

class MarmotBuiltins
{
private:
	inline static constexpr std::array s_signatures =
	{
#define MARMOT_BUILTIN(name, arg_kinds, return_kind) BuiltinSignature{ #name, arg_kinds, return_kind },
#include "Common/Builtins/Builtins.def"
#undef MARMOT_BUILTIN
	};

public:
	static constexpr int ABI_VERSION = 1;
	static constexpr size_t COUNT = s_signatures.size();

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

	static constexpr const BuiltinSignature& At(size_t index)
	{
		return s_signatures[index];
	}

	// Exiting the process is the one builtin a worker must not perform: it would
	// take the whole program down from a thread. The VM needs its position to
	// intercept the call, and the position is known here at compile time.
	static consteval size_t ExitIndex()
	{
		const std::optional<size_t> index = FindIndex("MIDORI_FFI_Exit");
		if (!index.has_value())
		{
			throw "MIDORI_FFI_Exit is missing from the builtin table";
		}
		return index.value();
	}
};
