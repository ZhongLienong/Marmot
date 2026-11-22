// Generated from format/mmc/registry.json. Do not edit.
// Regenerate: python scripts/dev.py generate-mmc

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace MmcWire
{
	inline constexpr std::array<uint8_t, 4u> MAGIC{ 0x4Du, 0x42u, 0x43u, 0x00u };
	inline constexpr uint32_t FORMAT_MARKER = 19u;
	inline constexpr std::size_t HEADER_SIZE = 32u;
	inline constexpr std::size_t MAGIC_OFFSET = 0u;
	inline constexpr std::size_t FORMAT_MARKER_OFFSET = 4u;
	inline constexpr std::size_t VERSION_MAJOR_OFFSET = 8u;
	inline constexpr std::size_t VERSION_MINOR_OFFSET = 10u;
	inline constexpr std::size_t VERSION_PATCH_OFFSET = 12u;
	inline constexpr std::size_t RESERVED_OFFSET = 14u;
	inline constexpr std::size_t FLAGS_OFFSET = 16u;
	inline constexpr std::size_t PAYLOAD_SIZE_OFFSET = 20u;
	inline constexpr std::size_t PAYLOAD_CRC32_OFFSET = 28u;
	inline constexpr uint32_t SOURCE_FILES_EMBEDDED_FLAG = 1u;

	inline constexpr uint32_t CRC32_POLYNOMIAL = 0xEDB88320u;
	inline constexpr uint32_t CRC32_INITIAL = 0xFFFFFFFFu;
	inline constexpr uint32_t CRC32_FINAL_XOR = 0xFFFFFFFFu;

	inline constexpr int FFI_ARGUMENT_BITS = 2;
	inline constexpr uint8_t FFI_ARGUMENT_MASK = 0x03u;
	inline constexpr uint8_t FFI_RETURN_KIND_MASK = 0x7Fu;
	inline constexpr uint8_t FFI_POINTER_ELEMENTS_FLAG = 0x80u;
	inline constexpr uint8_t FFI_ARGUMENT_RAW_VALUE = 0u;
	inline constexpr uint8_t FFI_ARGUMENT_C_STRING = 1u;
	inline constexpr uint8_t FFI_ARGUMENT_ARRAY_VIEW = 2u;
	inline constexpr uint8_t FFI_RETURN_RAW_VALUE = 0u;
	inline constexpr uint8_t FFI_RETURN_C_STRING = 1u;
	inline constexpr uint8_t FFI_RETURN_ARRAY_VALUES = 2u;
	inline constexpr uint8_t FFI_RETURN_ARRAY_STRINGS = 3u;

	inline constexpr int MAX_UNION_TAG = 255;
	inline constexpr int MAX_ARRAY_SIZE = 16777215;
	inline constexpr char MODULE_SEPARATOR = '@';
	inline constexpr std::string_view MAIN_PROCEDURE_PREFIX = "$main$";
	inline constexpr std::string_view MODULE_BOOTSTRAP_PREFIX = "$module_bootstrap$";
}
