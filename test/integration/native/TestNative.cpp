// A native library for the tests of `foreign ... from "library"`. Its functions
// use the Marmot FFI convention: arguments are 8-byte slots, and the result is
// written to `ret`.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define MARMOT_TEST_NATIVE_API extern "C" __declspec(dllexport)
#else
#define MARMOT_TEST_NATIVE_API extern "C" __attribute__((visibility("default")))
#endif

MARMOT_TEST_NATIVE_API void marmot_test_answer(void**, void* ret) noexcept
{
	const int64_t answer = 42;
	std::memcpy(ret, &answer, sizeof(answer));
}

MARMOT_TEST_NATIVE_API void marmot_test_add(void** args, void* ret) noexcept
{
	int64_t left = 0;
	int64_t right = 0;
	std::memcpy(&left, &args[0], sizeof(left));
	std::memcpy(&right, &args[1], sizeof(right));
	const int64_t sum = left + right;
	std::memcpy(ret, &sum, sizeof(sum));
}

struct MarmotTestArray
{
	void* data;
	int length;
};

MARMOT_TEST_NATIVE_API void marmot_test_length(void** args, void* ret) noexcept
{
	const int64_t length = static_cast<int64_t>(std::strlen(static_cast<const char*>(args[0])));
	std::memcpy(ret, &length, sizeof(length));
}

MARMOT_TEST_NATIVE_API void marmot_test_sum(void** args, void* ret) noexcept
{
	const MarmotTestArray* array = static_cast<const MarmotTestArray*>(args[0]);
	const int64_t* values = static_cast<const int64_t*>(array->data);
	int64_t sum = 0;
	for (int index = 0; index < array->length; index += 1)
	{
		sum += values[index];
	}
	std::memcpy(ret, &sum, sizeof(sum));
}

MARMOT_TEST_NATIVE_API void marmot_test_count(void** args, void* ret) noexcept
{
	int64_t count = 0;
	std::memcpy(&count, &args[0], sizeof(count));
	int64_t* values = static_cast<int64_t*>(std::malloc(static_cast<size_t>(count) * sizeof(int64_t)));
	for (int64_t index = 0; index < count; index += 1)
	{
		values[index] = index;
	}
	MarmotTestArray* array = static_cast<MarmotTestArray*>(std::malloc(sizeof(MarmotTestArray)));
	array->data = values;
	array->length = static_cast<int>(count);
	std::memcpy(ret, &array, sizeof(array));
}

MARMOT_TEST_NATIVE_API void marmot_test_shout(void** args, void* ret) noexcept
{
	const char* text = static_cast<const char*>(args[0]);
	const size_t length = std::strlen(text);
	char* result = static_cast<char*>(std::malloc(length + 2u));
	std::memcpy(result, text, length);
	result[length] = '!';
	result[length + 1u] = '\0';
	std::memcpy(ret, &result, sizeof(result));
}

MARMOT_TEST_NATIVE_API void marmot_test_words(void**, void* ret) noexcept
{
	char** words = static_cast<char**>(std::malloc(2u * sizeof(char*)));
	words[0] = static_cast<char*>(std::malloc(3u));
	std::memcpy(words[0], "up", 3u);
	words[1] = static_cast<char*>(std::malloc(5u));
	std::memcpy(words[1], "down", 5u);
	MarmotTestArray* array = static_cast<MarmotTestArray*>(std::malloc(sizeof(MarmotTestArray)));
	array->data = words;
	array->length = 2;
	std::memcpy(ret, &array, sizeof(array));
}
