// A native library for the tests of `foreign ... from "library"`. Its functions
// use the Marmot FFI convention: arguments are 8-byte slots, and the result is
// written to `ret`.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define MARMOT_FFI_IMPLEMENTATION
#include <marmot/MarmotFFI.h>

#include <atomic>

namespace
{
	std::atomic<int64_t> s_freed_results = 0;
	char s_up[] = "up";
	char s_down[] = "down";
	char* s_words[] = { s_up, s_down };
	MarmotFFIArray s_words_array{ s_words, 2 };
}

#ifndef MARMOT_TEST_NO_FREE_RESULT
MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t kind, void* ptr)
{
	s_freed_results += 1;
	if (kind == MARMOT_FFI_RETURN_ARRAY_STRINGS && ptr == &s_words_array)
	{
		// This result uses storage only this library can release. Overwrite it
		// so clients also have to copy the strings before the deallocation call.
		s_up[0] = 'X';
		return;
	}
	if (kind == MARMOT_FFI_RETURN_ARRAY_VALUES)
	{
		std::free(static_cast<MarmotFFIArray*>(ptr)->data);
	}
	std::free(ptr);
}
#endif

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_freed(void**, void* ret)
{
	const int64_t freed = s_freed_results.load();
	std::memcpy(ret, &freed, sizeof(freed));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_text_bytes(void** args, void* ret)
{
	const MarmotFFIArray& array = *static_cast<const MarmotFFIArray*>(args[0]);
	const auto* strings = static_cast<const char* const*>(array.data);
	int64_t bytes = 0;
	for (int32_t index = 0; index < array.length; index += 1)
	{
		bytes += static_cast<int64_t>(std::strlen(strings[index]));
	}
	std::memcpy(ret, &bytes, sizeof(bytes));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_answer(void**, void* ret) noexcept
{
	const int64_t answer = 42;
	std::memcpy(ret, &answer, sizeof(answer));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_add(void** args, void* ret) noexcept
{
	int64_t left = 0;
	int64_t right = 0;
	std::memcpy(&left, &args[0], sizeof(left));
	std::memcpy(&right, &args[1], sizeof(right));
	const int64_t sum = left + right;
	std::memcpy(ret, &sum, sizeof(sum));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_length(void** args, void* ret) noexcept
{
	const int64_t length = static_cast<int64_t>(std::strlen(static_cast<const char*>(args[0])));
	std::memcpy(ret, &length, sizeof(length));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_sum(void** args, void* ret) noexcept
{
	const MarmotFFIArray* array = static_cast<const MarmotFFIArray*>(args[0]);
	const int64_t* values = static_cast<const int64_t*>(array->data);
	int64_t sum = 0;
	for (int index = 0; index < array->length; index += 1)
	{
		sum += values[index];
	}
	std::memcpy(ret, &sum, sizeof(sum));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_count(void** args, void* ret) noexcept
{
	int64_t count = 0;
	std::memcpy(&count, &args[0], sizeof(count));
	int64_t* values = static_cast<int64_t*>(std::malloc(static_cast<size_t>(count) * sizeof(int64_t)));
	for (int64_t index = 0; index < count; index += 1)
	{
		values[index] = index;
	}
	MarmotFFIArray* array = static_cast<MarmotFFIArray*>(std::malloc(sizeof(MarmotFFIArray)));
	array->data = values;
	array->length = static_cast<int>(count);
	std::memcpy(ret, &array, sizeof(array));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_shout(void** args, void* ret) noexcept
{
	const char* text = static_cast<const char*>(args[0]);
	const size_t length = std::strlen(text);
	char* result = static_cast<char*>(std::malloc(length + 2u));
	std::memcpy(result, text, length);
	result[length] = '!';
	result[length + 1u] = '\0';
	std::memcpy(ret, &result, sizeof(result));
}

extern "C" MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_test_words(void**, void* ret) noexcept
{
	s_up[0] = 'u';
	MarmotFFIArray* array = &s_words_array;
	std::memcpy(ret, &array, sizeof(array));
}
