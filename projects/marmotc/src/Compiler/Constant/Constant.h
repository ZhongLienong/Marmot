// Marmot Compiler Constants

#pragma once

#include <cinttypes>
#include <cstddef>

inline constexpr int MAX_CAPTURED_COUNT{ UINT8_MAX };

inline constexpr int MAX_SIZE_OP_CONSTANT_LONG{ UINT16_MAX };

inline constexpr int MAX_LOCAL_VARIABLES{ UINT8_MAX };

inline constexpr int MAX_VARIABLES{ UINT16_MAX };

inline constexpr int IMPORT_PLACEHOLDER_BASE{ 0xF000 };

inline constexpr int MAX_IMPORT_PLACEHOLDERS{ 0x1000 };

inline constexpr int MAX_JUMP_SIZE{ UINT16_MAX };

inline constexpr int MAX_FUNCTION_ARITY{ UINT8_MAX };

inline constexpr size_t MAX_NATIVE_ARITY{ 4uz };

// Byte manipulation constants
inline constexpr uint8_t BYTE_MASK = 0xffu;

// Bit shift amounts for byte extraction
inline constexpr unsigned int SHIFT_8_BITS = 8u;
inline constexpr unsigned int SHIFT_16_BITS = 16u;

// Hash function constants (FNV-1a)
inline constexpr uint32_t HASH_OFFSET_BASIS = 0x9e3779b9u;
inline constexpr unsigned int HASH_LEFT_SHIFT = 6u;
inline constexpr unsigned int HASH_RIGHT_SHIFT = 2u;
