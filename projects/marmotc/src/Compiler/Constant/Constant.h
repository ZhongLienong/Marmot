// Marmot Compiler Constants

#pragma once

#include <cinttypes>
#include <string>
#include <string_view>

inline constexpr int MAX_CAPTURED_COUNT{ UINT8_MAX };

inline constexpr int MAX_SIZE_OP_CONSTANT_LONG{ UINT16_MAX };

inline constexpr int MAX_LOCAL_VARIABLES{ UINT8_MAX };

inline constexpr int MAX_VARIABLES{ UINT16_MAX };

inline constexpr int IMPORT_PLACEHOLDER_BASE{ 0xF000 };

inline constexpr int MAX_IMPORT_PLACEHOLDERS{ 0x1000 };

inline constexpr int MAX_JUMP_SIZE{ UINT16_MAX };

inline constexpr int MAX_FUNCTION_ARITY{ UINT8_MAX };

constexpr std::string_view NameSeparator = "::";

constexpr char INTERNAL_NAME_PREFIX = '$';

// Byte manipulation constants
inline constexpr uint8_t BYTE_MASK = 0xffu;

// Bit shift amounts for byte extraction
inline constexpr unsigned int SHIFT_8_BITS = 8u;
inline constexpr unsigned int SHIFT_16_BITS = 16u;

// Hash function constants (FNV-1a)
inline constexpr uint32_t HASH_OFFSET_BASIS = 0x9e3779b9u;
inline constexpr unsigned int HASH_LEFT_SHIFT = 6u;
inline constexpr unsigned int HASH_RIGHT_SHIFT = 2u;

// Typeclass names
constexpr std::string_view CONVERTABLE_CLASS_NAME = "Convertable";
constexpr std::string_view CONVERT_METHOD_NAME = "Convert";

constexpr std::string_view EQUATABLE_CLASS_NAME = "Equatable";
constexpr std::string_view EQUALS_METHOD_NAME = "Equals";

constexpr std::string_view ORDERABLE_CLASS_NAME = "Orderable";
constexpr std::string_view COMPARE_METHOD_NAME = "Compare";

constexpr std::string_view CONCATENABLE_CLASS_NAME = "Concatenable";
constexpr std::string_view CONCAT_METHOD_NAME = "Concat";

constexpr std::string_view COUNTABLE_CLASS_NAME = "Countable";
constexpr std::string_view COUNT_METHOD_NAME = "Count";

constexpr std::string_view ITERABLE_CLASS_NAME = "Iterable";
constexpr std::string_view NEXT_METHOD_NAME = "Next";

constexpr std::string_view INDEXABLE_CLASS_NAME = "Indexable";
constexpr std::string_view GET_METHOD_NAME = "Get";
constexpr std::string_view ELEMENT_ASSOCIATED_TYPE_NAME = "Element";
