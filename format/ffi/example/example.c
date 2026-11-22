#define MARMOT_FFI_IMPLEMENTATION
#include <marmot/MarmotFFI.h>

#include <stdlib.h>
#include <string.h>

MARMOT_FFI_EXPORT void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr)
{
    if (return_kind == MARMOT_FFI_RETURN_ARRAY_VALUES || return_kind == MARMOT_FFI_RETURN_ARRAY_STRINGS) {
        MarmotFFIArray* array = ptr;
        if (return_kind == MARMOT_FFI_RETURN_ARRAY_STRINGS) {
            char** strings = array->data;
            for (int32_t i = 0; i < array->length; ++i) {
                free(strings[i]);
            }
        }
        free(array->data);
    }
    free(ptr);
}

MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_example_count(void** args, void* ret)
{
    const MarmotFFIArray* words = args[0];
    const char* const* strings = words->data;
    int64_t bytes = 0;
    for (int32_t i = 0; i < words->length; ++i) {
        bytes += (int64_t)strlen(strings[i]);
    }
    memcpy(ret, &bytes, sizeof(bytes));
}

MARMOT_FFI_EXPORT void MARMOT_FFI_CALL marmot_example_copy(void** args, void* ret)
{
    const char* text = args[0];
    const size_t size = strlen(text) + 1;
    char* result = malloc(size);
    if (result != NULL) {
        memcpy(result, text, size);
    }
    memcpy(ret, &result, sizeof(result));
}
