#pragma once

#if defined(_MSC_VER)
#define MARMOT_FORCE_INLINE __forceinline
#define MARMOT_NOINLINE __declspec(noinline)
#else
#define MARMOT_FORCE_INLINE __attribute__((always_inline)) inline
#define MARMOT_NOINLINE __attribute__((noinline))
#endif
