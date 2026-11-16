#pragma once

#include <string_view>

constexpr std::string_view NameSeparator = "::";

constexpr char INTERNAL_NAME_PREFIX = '$';

// The library declarations language features are bound to: operators, `as`,
// `for`, indexing, deriving and transfer between workers. A class's name is its
// identity in a program: a module can neither declare a class of a name it imports
// nor import two different classes of one name. So this table is the one place the
// compiler names a library declaration.
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

constexpr std::string_view HASHABLE_CLASS_NAME = "Hashable";
constexpr std::string_view HASH_METHOD_NAME = "Hash";

constexpr std::string_view ITERABLE_CLASS_NAME = "Iterable";
constexpr std::string_view NEXT_METHOD_NAME = "Next";
constexpr std::string_view ITEM_ASSOCIATED_TYPE_NAME = "Item";
constexpr std::string_view ITERABLE_SOME_CONSTRUCTOR_NAME = "Some";

constexpr std::string_view NUMERIC_CLASS_NAME = "Numeric";
constexpr std::string_view BITWISE_CLASS_NAME = "Bitwise";

// Decided from a type's structure, not only by its instances: a value can cross
// to another worker exactly when EnsureTransferable says so.
constexpr std::string_view TRANSFERABLE_CLASS_NAME = "Transferable";

constexpr std::string_view INDEXABLE_CLASS_NAME = "Indexable";
constexpr std::string_view GET_METHOD_NAME = "Get";
constexpr std::string_view ELEMENT_ASSOCIATED_TYPE_NAME = "Element";

// Container-deriving targets also form generated function suffixes, as in OptionBoxMap.
constexpr std::string_view MAP_DERIVING_NAME = "Map";
constexpr std::string_view BIND_DERIVING_NAME = "Bind";
constexpr std::string_view UNWRAP_DERIVING_NAME = "Unwrap";
