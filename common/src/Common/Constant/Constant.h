// Marmot Compiler Constants

#pragma once

#include <cinttypes>
#include <string>
#include <string_view>

inline constexpr int MAX_CAPTURED_COUNT{ UINT8_MAX };

inline constexpr int MAX_SIZE_OP_CONSTANT{ UINT8_MAX };

inline constexpr int MAX_SIZE_OP_CONSTANT_LONG{ UINT16_MAX };

inline constexpr int MAX_SIZE_OP_CONSTANT_LONG_LONG{ ((UINT16_MAX << 8) | 0xffff) };

inline constexpr int MAX_LOCAL_VARIABLES{ UINT8_MAX };

inline constexpr int MAX_VARIABLES{ UINT16_MAX };

inline constexpr int IMPORT_PLACEHOLDER_BASE{ 0xF000 };

inline constexpr int MAX_IMPORT_PLACEHOLDERS{ 0x1000 };

inline constexpr int MAX_JUMP_SIZE{ UINT16_MAX };

inline constexpr int MAX_FUNCTION_ARITY{ UINT8_MAX };

inline constexpr int MAX_FUNCTION_COUNT{ UINT8_MAX };

inline constexpr int MAX_UNION_TAG{ UINT8_MAX };

inline constexpr int MAX_ARRAY_SIZE{ ((UINT16_MAX << 8) | 0xffff) };

constexpr std::string_view NameSeparator = "::";

constexpr char ModuleSeparator = '@';
constexpr char INTERNAL_NAME_PREFIX = '$';

// Byte manipulation constants
inline constexpr uint8_t BYTE_MASK = 0xffu;
inline constexpr int BYTE_SIZE_BITS = 8;
inline constexpr int SHORT_SIZE_BITS = 16;
inline constexpr int TRIBYTE_SIZE_BITS = 24;

// Bit shift amounts for byte extraction
inline constexpr unsigned int SHIFT_8_BITS = 8u;
inline constexpr unsigned int SHIFT_16_BITS = 16u;
inline constexpr unsigned int SHIFT_24_BITS = 24u;
inline constexpr unsigned int SHIFT_32_BITS = 32u;
inline constexpr unsigned int SHIFT_40_BITS = 40u;
inline constexpr unsigned int SHIFT_48_BITS = 48u;
inline constexpr unsigned int SHIFT_56_BITS = 56u;

// Hash function constants (FNV-1a)
inline constexpr uint32_t HASH_OFFSET_BASIS = 0x9e3779b9u;
inline constexpr unsigned int HASH_LEFT_SHIFT = 6u;
inline constexpr unsigned int HASH_RIGHT_SHIFT = 2u;

// Main procedure naming
constexpr std::string_view MAIN_PROCEDURE_PREFIX = "$main$";
constexpr std::string_view MODULE_BOOTSTRAP_PREFIX = "$module_bootstrap$";
constexpr std::string_view MODULE_SEPARATOR_STR = "@";

// For-loop hidden variable prefixes
constexpr std::string_view FOR_STEP_PREFIX = "$for_step_";
constexpr std::string_view FOR_END_PREFIX = "$for_end_";
constexpr std::string_view FOR_ARRAY_PREFIX = "$for_array_";

// Array comprehension hidden variable prefix
constexpr std::string_view COMPREHENSION_RESULT_PREFIX = "$comp_result_";

// Match expression hidden variable prefix
constexpr std::string_view MATCH_VALUE_PREFIX = "$match_value_";

// Typeclass names
constexpr std::string_view CONVERTABLE_CLASS_NAME = "Convertable";
constexpr std::string_view CONVERT_METHOD_NAME = "Convert";
constexpr std::string_view CONVERT_MANGLED_PREFIX = "Convert_Convertable_";

constexpr std::string_view EQUATABLE_CLASS_NAME = "Equatable";
constexpr std::string_view EQUALS_METHOD_NAME = "Equals";
constexpr std::string_view EQUALS_MANGLED_PREFIX = "Equals_Equatable_";

constexpr std::string_view ORDERABLE_CLASS_NAME = "Orderable";
constexpr std::string_view COMPARE_METHOD_NAME = "Compare";
constexpr std::string_view COMPARE_MANGLED_PREFIX = "Compare_Orderable_";

constexpr std::string_view CONCATENABLE_CLASS_NAME = "Concatenable";
constexpr std::string_view CONCAT_METHOD_NAME = "Concat";
constexpr std::string_view CONCAT_MANGLED_PREFIX = "Concat_Concatenable_";

constexpr std::string_view COUNTABLE_CLASS_NAME = "Countable";
constexpr std::string_view COUNT_METHOD_NAME = "Count";
constexpr std::string_view COUNT_MANGLED_PREFIX = "Count_Countable_";

constexpr std::string_view ITERABLE_CLASS_NAME = "Iterable";
constexpr std::string_view NEXT_METHOD_NAME = "Next";
constexpr std::string_view NEXT_MANGLED_PREFIX = "Next_Iterable_";

constexpr std::string_view INDEXABLE_CLASS_NAME = "Indexable";
constexpr std::string_view GET_METHOD_NAME = "Get";
constexpr std::string_view GET_MANGLED_PREFIX = "Get_Indexable_";
constexpr std::string_view ELEMENT_ASSOCIATED_TYPE_NAME = "Element";

// Standard library paths
constexpr std::string_view STDLIB_DLL_NAME = "MidoriStdLib.dll";
constexpr std::string_view STDLIB_SO_NAME = "libMidoriStdLib.so";
constexpr std::string_view LIBRARY_PATH_PREFIX = "./";
constexpr char STDLIB_DLL_PATH[] = "./MidoriStdLib.dll";
constexpr char STDLIB_SO_PATH[] = "./libMidoriStdLib.so";

// Thread pool
inline constexpr unsigned int DEFAULT_THREAD_POOL_SIZE = 4u;

// Stack trace formatting
constexpr std::string_view STACK_TRACE_HEADER = "Stack trace:\n";
constexpr std::string_view ANONYMOUS_FUNCTION = "<anonymous>";
constexpr std::string_view STACK_FRAME_FORMAT = "  at {}{}{}";
constexpr std::string_view TRUNCATED_FRAMES_FORMAT = "  ... ({} more frame{})";
constexpr std::string_view STDLIB_LOAD_ERROR = "Failed to load the standard library.";
