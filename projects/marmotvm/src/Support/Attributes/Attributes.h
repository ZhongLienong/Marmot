#pragma once

#if defined(_MSC_VER)
#define MIDORI_FORCE_INLINE __forceinline
#define MIDORI_NOINLINE __declspec(noinline)
#else
#define MIDORI_FORCE_INLINE __attribute__((always_inline)) inline
#define MIDORI_NOINLINE __attribute__((noinline))
#endif
