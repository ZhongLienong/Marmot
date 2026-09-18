#pragma once

#include "Library/MidoriStdLibExports.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

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

class MidoriFFIRegistry
{
private:
	inline static constexpr std::array s_entries =
	{
		FFIEntry{ "MIDORI_FFI_Print", &MIDORI_FFI_Print, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_PrintError", &MIDORI_FFI_PrintError, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_ReadInput", &MIDORI_FFI_ReadInput, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_ReadLine", &MIDORI_FFI_ReadLine, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_ReadFile", &MIDORI_FFI_ReadFile, MakeFFIArgKinds(FFIArgumentKind::CString), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_WriteFile", &MIDORI_FFI_WriteFile, MakeFFIArgKinds(FFIArgumentKind::CString, FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_AppendToFile", &MIDORI_FFI_AppendToFile, MakeFFIArgKinds(FFIArgumentKind::CString, FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_ReadBinaryFile", &MIDORI_FFI_ReadBinaryFile, MakeFFIArgKinds(FFIArgumentKind::CString), FFIReturnKind::ArrayValues },
		FFIEntry{ "MIDORI_FFI_WriteBinaryFile", &MIDORI_FFI_WriteBinaryFile, MakeFFIArgKinds(FFIArgumentKind::CString, FFIArgumentKind::ArrayView) },
		FFIEntry{ "MIDORI_FFI_FileExists", &MIDORI_FFI_FileExists, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_DeleteFile", &MIDORI_FFI_DeleteFile, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_RenameFile", &MIDORI_FFI_RenameFile, MakeFFIArgKinds(FFIArgumentKind::CString, FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_GetFileSize", &MIDORI_FFI_GetFileSize, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_GetLastIOErrorKind", &MIDORI_FFI_GetLastIOErrorKind },
		FFIEntry{ "MIDORI_FFI_GetLastIOErrorMessage", &MIDORI_FFI_GetLastIOErrorMessage, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_SquareRoot", &MIDORI_FFI_SquareRoot },
		FFIEntry{ "MIDORI_FFI_Abs", &MIDORI_FFI_Abs },
		FFIEntry{ "MIDORI_FFI_Pow", &MIDORI_FFI_Pow },
		FFIEntry{ "MIDORI_FFI_Exp", &MIDORI_FFI_Exp },
		FFIEntry{ "MIDORI_FFI_Log", &MIDORI_FFI_Log },
		FFIEntry{ "MIDORI_FFI_Log10", &MIDORI_FFI_Log10 },
		FFIEntry{ "MIDORI_FFI_Log2", &MIDORI_FFI_Log2 },
		FFIEntry{ "MIDORI_FFI_Sin", &MIDORI_FFI_Sin },
		FFIEntry{ "MIDORI_FFI_Cos", &MIDORI_FFI_Cos },
		FFIEntry{ "MIDORI_FFI_Tan", &MIDORI_FFI_Tan },
		FFIEntry{ "MIDORI_FFI_Asin", &MIDORI_FFI_Asin },
		FFIEntry{ "MIDORI_FFI_Acos", &MIDORI_FFI_Acos },
		FFIEntry{ "MIDORI_FFI_Atan", &MIDORI_FFI_Atan },
		FFIEntry{ "MIDORI_FFI_Atan2", &MIDORI_FFI_Atan2 },
		FFIEntry{ "MIDORI_FFI_Sinh", &MIDORI_FFI_Sinh },
		FFIEntry{ "MIDORI_FFI_Cosh", &MIDORI_FFI_Cosh },
		FFIEntry{ "MIDORI_FFI_Tanh", &MIDORI_FFI_Tanh },
		FFIEntry{ "MIDORI_FFI_Floor", &MIDORI_FFI_Floor },
		FFIEntry{ "MIDORI_FFI_Ceil", &MIDORI_FFI_Ceil },
		FFIEntry{ "MIDORI_FFI_Round", &MIDORI_FFI_Round },
		FFIEntry{ "MIDORI_FFI_Trunc", &MIDORI_FFI_Trunc },
		FFIEntry{ "MIDORI_FFI_Min", &MIDORI_FFI_Min },
		FFIEntry{ "MIDORI_FFI_Max", &MIDORI_FFI_Max },
		FFIEntry{ "MIDORI_FFI_Clamp", &MIDORI_FFI_Clamp },
		FFIEntry{ "MIDORI_FFI_Sign", &MIDORI_FFI_Sign },
		FFIEntry{ "MIDORI_FFI_Fmod", &MIDORI_FFI_Fmod },
		FFIEntry{ "MIDORI_FFI_Hypot", &MIDORI_FFI_Hypot },
		FFIEntry{ "MIDORI_FFI_Random", &MIDORI_FFI_Random },
		FFIEntry{ "MIDORI_FFI_RandomInt", &MIDORI_FFI_RandomInt },
		FFIEntry{ "MIDORI_FFI_RandomFloat", &MIDORI_FFI_RandomFloat },
		FFIEntry{ "MIDORI_FFI_ToRadians", &MIDORI_FFI_ToRadians },
		FFIEntry{ "MIDORI_FFI_ToDegrees", &MIDORI_FFI_ToDegrees },
		FFIEntry{ "MIDORI_FFI_IsNaN", &MIDORI_FFI_IsNaN },
		FFIEntry{ "MIDORI_FFI_IsInf", &MIDORI_FFI_IsInf },
		FFIEntry{ "MIDORI_FFI_IsFinite", &MIDORI_FFI_IsFinite },
		FFIEntry{ "MIDORI_FFI_GetTime", &MIDORI_FFI_GetTime },
		FFIEntry{ "MIDORI_FFI_GetYear", &MIDORI_FFI_GetYear },
		FFIEntry{ "MIDORI_FFI_GetMonth", &MIDORI_FFI_GetMonth },
		FFIEntry{ "MIDORI_FFI_GetDay", &MIDORI_FFI_GetDay },
		FFIEntry{ "MIDORI_FFI_GetHour", &MIDORI_FFI_GetHour },
		FFIEntry{ "MIDORI_FFI_GetMinute", &MIDORI_FFI_GetMinute },
		FFIEntry{ "MIDORI_FFI_GetSecond", &MIDORI_FFI_GetSecond },
		FFIEntry{ "MIDORI_FFI_GetDayOfWeek", &MIDORI_FFI_GetDayOfWeek },
		FFIEntry{ "MIDORI_FFI_GetDayOfYear", &MIDORI_FFI_GetDayOfYear },
		FFIEntry{ "MIDORI_FFI_FormatTime", &MIDORI_FFI_FormatTime, MakeFFIArgKinds(FFIArgumentKind::CString), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_GetTimezoneOffset", &MIDORI_FFI_GetTimezoneOffset },
		FFIEntry{ "MIDORI_FFI_GetUtcYear", &MIDORI_FFI_GetUtcYear },
		FFIEntry{ "MIDORI_FFI_GetUtcMonth", &MIDORI_FFI_GetUtcMonth },
		FFIEntry{ "MIDORI_FFI_GetUtcDay", &MIDORI_FFI_GetUtcDay },
		FFIEntry{ "MIDORI_FFI_GetUtcHour", &MIDORI_FFI_GetUtcHour },
		FFIEntry{ "MIDORI_FFI_GetUtcMinute", &MIDORI_FFI_GetUtcMinute },
		FFIEntry{ "MIDORI_FFI_GetUtcSecond", &MIDORI_FFI_GetUtcSecond },
		FFIEntry{ "MIDORI_FFI_GetUtcDayOfWeek", &MIDORI_FFI_GetUtcDayOfWeek },
		FFIEntry{ "MIDORI_FFI_GetUtcDayOfYear", &MIDORI_FFI_GetUtcDayOfYear },
		FFIEntry{ "MIDORI_FFI_Exit", &MIDORI_FFI_Exit },
		FFIEntry{ "MIDORI_FFI_GetEnv", &MIDORI_FFI_GetEnv, MakeFFIArgKinds(FFIArgumentKind::CString), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_SetEnv", &MIDORI_FFI_SetEnv, MakeFFIArgKinds(FFIArgumentKind::CString, FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_Sleep", &MIDORI_FFI_Sleep },
		FFIEntry{ "MIDORI_FFI_GetCurrentDirectory", &MIDORI_FFI_GetCurrentDirectory, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_SetCurrentDirectory", &MIDORI_FFI_SetCurrentDirectory, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_Execute", &MIDORI_FFI_Execute, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_GetPlatform", &MIDORI_FFI_GetPlatform, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_GetProcessId", &MIDORI_FFI_GetProcessId },
		FFIEntry{ "MIDORI_FFI_GetLastSystemErrorKind", &MIDORI_FFI_GetLastSystemErrorKind },
		FFIEntry{ "MIDORI_FFI_GetLastSystemErrorMessage", &MIDORI_FFI_GetLastSystemErrorMessage, {}, FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_ArrayConcat", &MIDORI_FFI_ArrayConcat, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle), FFIReturnKind::ArrayValues },
		FFIEntry{ "MIDORI_FFI_ArrayLength", &MIDORI_FFI_ArrayLength, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle) },
		FFIEntry{ "MIDORI_FFI_ArraySlice", &MIDORI_FFI_ArraySlice, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::ValueHandle, FFIArgumentKind::ValueHandle), FFIReturnKind::ArrayValues },
		FFIEntry{ "MIDORI_FFI_ArrayReverse", &MIDORI_FFI_ArrayReverse, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle), FFIReturnKind::ArrayValues },
		FFIEntry{ "MIDORI_FFI_ArrayContains", &MIDORI_FFI_ArrayContains, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::ValueHandle) },
		FFIEntry{ "MIDORI_FFI_TextConcat", &MIDORI_FFI_TextConcat, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_TextLength", &MIDORI_FFI_TextLength, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_TextSubstring", &MIDORI_FFI_TextSubstring, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::ValueHandle, FFIArgumentKind::ValueHandle), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_TextSplit", &MIDORI_FFI_TextSplit, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle), FFIReturnKind::ArrayStrings },
		FFIEntry{ "MIDORI_FFI_TextReverse", &MIDORI_FFI_TextReverse, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_TextContains", &MIDORI_FFI_TextContains, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle) },
		FFIEntry{ "MIDORI_FFI_TextReplace", &MIDORI_FFI_TextReplace, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle, FFIArgumentKind::TraceableHandle), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_TextTrim", &MIDORI_FFI_TextTrim, MakeFFIArgKinds(FFIArgumentKind::TraceableHandle), FFIReturnKind::CString },
		FFIEntry{ "MIDORI_FFI_HashText", &MIDORI_FFI_HashText, MakeFFIArgKinds(FFIArgumentKind::CString) },
		FFIEntry{ "MIDORI_FFI_HashFloat", &MIDORI_FFI_HashFloat },
	};

public:
	static constexpr int ABI_VERSION = 1;
	static constexpr size_t BUILTIN_COUNT = s_entries.size();

	// Exiting the process is the one builtin a worker must not perform: it would
	// take the whole program down from a thread. The VM needs its position to
	// intercept the call, and the position is known here at compile time.
	static consteval size_t ExitBuiltinIndex()
	{
		for (size_t index = 0u; index < s_entries.size(); index += 1u)
		{
			if (std::string_view(s_entries[index].m_name) == std::string_view("MIDORI_FFI_Exit"))
			{
				return index;
			}
		}

		throw "MIDORI_FFI_Exit is missing from the builtin table";
	}

	static const FFIEntry& GetEntry(size_t index);
	static std::optional<size_t> FindIndex(std::string_view name);
	static constexpr size_t GetTableSize();
	static const std::array<FFIEntry, BUILTIN_COUNT>& GetTable();
};
