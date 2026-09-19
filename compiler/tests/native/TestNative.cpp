// A native library for the tests of `foreign ... from "library"`. Its functions
// use the Marmot FFI convention: arguments are 8-byte slots, and the result is
// written to `ret`.

#include <cstdint>
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
