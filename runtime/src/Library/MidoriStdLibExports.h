#pragma once

#include <cstdint>

#if defined(_WIN32) || defined(_WIN64)
#define MIDORI_STDLIB_API __declspec(dllexport)
#else
#define MIDORI_STDLIB_API
#endif

#define MIDORI_FFI_FUNC(name) MIDORI_FFI_##name

extern "C"
{
	// IO - Console
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Print)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(PrintError)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadInput)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadLine)(void** args, void* ret) noexcept;

	// IO - File Operations
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(WriteFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(AppendToFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ReadBinaryFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(WriteBinaryFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(FileExists)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(DeleteFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(RenameFile)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetFileSize)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastIOErrorKind)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastIOErrorMessage)(void** args, void* ret) noexcept;

	// Math - Basic
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SquareRoot)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Abs)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Pow)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Exp)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Log)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Log10)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Log2)(void** args, void* ret) noexcept;

	// Math - Trigonometric
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Sin)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Cos)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Tan)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Asin)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Acos)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Atan)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Atan2)(void** args, void* ret) noexcept;

	// Math - Hyperbolic
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Sinh)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Cosh)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Tanh)(void** args, void* ret) noexcept;

	// Math - Rounding
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Floor)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Ceil)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Round)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Trunc)(void** args, void* ret) noexcept;

	// Math - Other
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Min)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Max)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Clamp)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Sign)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Fmod)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Hypot)(void** args, void* ret) noexcept;

	// Math - Random
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Random)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(RandomInt)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(RandomFloat)(void** args, void* ret) noexcept;

	// Math - Conversion
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ToRadians)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ToDegrees)(void** args, void* ret) noexcept;

	// Math - Special
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(IsNaN)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(IsInf)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(IsFinite)(void** args, void* ret) noexcept;

	// DateTime
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetTime)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetYear)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetMonth)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDay)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetHour)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetMinute)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetSecond)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDayOfWeek)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetDayOfYear)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(FormatTime)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetTimezoneOffset)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcYear)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcMonth)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDay)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcHour)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcMinute)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcSecond)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDayOfWeek)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetUtcDayOfYear)(void** args, void* ret) noexcept;

	// System
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Exit)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetEnv)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SetEnv)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Sleep)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetCurrentDirectory)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(SetCurrentDirectory)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(Execute)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetPlatform)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetProcessId)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastSystemErrorKind)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(GetLastSystemErrorMessage)(void** args, void* ret) noexcept;

	// Built-in Arrays
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayConcat)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayLength)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArraySlice)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayReverse)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(ArrayContains)(void** args, void* ret) noexcept;

	// Text
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextConcat)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextLength)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextSubstring)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextSplit)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextReverse)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextContains)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextReplace)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(TextTrim)(void** args, void* ret) noexcept;

	// Hashing
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(HashText)(void** args, void* ret) noexcept;
	MIDORI_STDLIB_API void MIDORI_FFI_FUNC(HashFloat)(void** args, void* ret) noexcept;
}
