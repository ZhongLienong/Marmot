#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define EXAMPLE_EXPORT __declspec(dllexport)
#define EXAMPLE_CALL __cdecl
#else
#define EXAMPLE_EXPORT __attribute__((visibility("default")))
#define EXAMPLE_CALL
#endif

struct ArrayRecord {
    void* data;
    int32_t length;
};

EXAMPLE_EXPORT void EXAMPLE_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr)
{
    // This library's only owned result is text.
    (void)return_kind;
    free(ptr);
}

EXAMPLE_EXPORT void EXAMPLE_CALL marmot_example_count(void** args, void* ret)
{
    struct ArrayRecord words;
    memcpy(&words, args[0], sizeof(words));
    const char* const* strings = words.data;
    int64_t bytes = 0;
    for (int32_t i = 0; i < words.length; ++i) {
        bytes += (int64_t)strlen(strings[i]);
    }
    memcpy(ret, &bytes, sizeof(bytes));
}

EXAMPLE_EXPORT void EXAMPLE_CALL marmot_example_copy(void** args, void* ret)
{
    const char* text = args[0];
    const size_t size = strlen(text) + 1;
    char* result = malloc(size);
    if (result != NULL) {
        memcpy(result, text, size);
    }
    memcpy(ret, &result, sizeof(result));
}
