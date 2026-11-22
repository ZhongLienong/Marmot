#ifndef MARMOT_FFI_H
#define MARMOT_FFI_H

#include <stdint.h>

#if defined(_WIN32)
#define MARMOT_FFI_CALL __cdecl
#define MARMOT_FFI_EXPORT __declspec(dllexport)
#else
#define MARMOT_FFI_CALL
#define MARMOT_FFI_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MarmotFFIArray {
    void* data;
    int32_t length;
} MarmotFFIArray;

typedef void (MARMOT_FFI_CALL *MarmotFFIFn)(void** args, void* ret);
typedef void (MARMOT_FFI_CALL *MarmotFFIFreeResultFn)(uint8_t return_kind, void* ptr);

enum MarmotFFIReturnKind {
    MARMOT_FFI_RETURN_SCALAR = 0,
    MARMOT_FFI_RETURN_STRING = 1,
    MARMOT_FFI_RETURN_ARRAY_VALUES = 2,
    MARMOT_FFI_RETURN_ARRAY_STRINGS = 3
};

#if defined(MARMOT_FFI_IMPLEMENTATION)
MARMOT_FFI_EXPORT
#endif
void MARMOT_FFI_CALL MARMOT_FFI_FreeResult(uint8_t return_kind, void* ptr);

#ifdef __cplusplus
}
#endif

#endif
