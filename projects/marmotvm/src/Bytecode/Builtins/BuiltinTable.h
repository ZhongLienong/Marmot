#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "Bytecode/Format/Format.h"

// VM-side builtin IDs and signatures in .mmc order. The compiler keeps its
// own table; the v14 registry check requires both tables to agree.

using VmFFIFunction = void(*)(void** args, void* ret);

constexpr size_t VM_FFI_MAX_ARITY = 4u;

enum class VmFFIArgumentKind : uint8_t
{
	RawValue = MmcWire::FFI_ARGUMENT_RAW_VALUE,
	CString = MmcWire::FFI_ARGUMENT_C_STRING,
	ArrayView = MmcWire::FFI_ARGUMENT_ARRAY_VIEW,
	TraceableHandle,
	ValueHandle
};

enum class VmFFIReturnKind : uint8_t
{
	RawValue = MmcWire::FFI_RETURN_RAW_VALUE,
	CString = MmcWire::FFI_RETURN_C_STRING,
	ArrayValues = MmcWire::FFI_RETURN_ARRAY_VALUES,
	ArrayStrings = MmcWire::FFI_RETURN_ARRAY_STRINGS,
	Value
};

template<typename... Kinds>
consteval std::array<VmFFIArgumentKind, VM_FFI_MAX_ARITY> MakeVmFFIArgKinds(Kinds... kinds)
{
	static_assert(sizeof...(Kinds) <= VM_FFI_MAX_ARITY);
	std::array<VmFFIArgumentKind, VM_FFI_MAX_ARITY> result{};
	VmFFIArgumentKind values[] = { kinds... };
	for (size_t i = 0uz; i < sizeof...(Kinds); i += 1uz)
	{
		result[i] = values[i];
	}
	return result;
}

struct VmBuiltinSignature
{
	std::string_view m_name;
	std::array<VmFFIArgumentKind, VM_FFI_MAX_ARITY> m_arg_kinds{};
	VmFFIReturnKind m_return_kind = VmFFIReturnKind::RawValue;
};

class VmBuiltins
{
private:
	inline static constexpr std::array s_signatures =
	{
#define MARMOT_BUILTIN(name, arg_kinds, return_kind) VmBuiltinSignature{ #name, arg_kinds, return_kind },
#include "Bytecode/Builtins/Builtins.def"
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

	static constexpr const VmBuiltinSignature& At(size_t index)
	{
		return s_signatures[index];
	}

	// Exiting the process is the one builtin a worker must not perform: it would
	// take the whole program down from a thread. The VM needs its position to
	// intercept the call, and the position is known here at compile time.
	static consteval size_t ExitIndex()
	{
		const std::optional<size_t> index = FindIndex("MARMOT_FFI_Exit");
		if (!index.has_value())
		{
			throw "MARMOT_FFI_Exit is missing from the builtin table";
		}
		return index.value();
	}
};
