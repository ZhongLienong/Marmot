#pragma once

#include <cstdint>
#include <string_view>

// What marmotc writes and marmotvm reads must agree on besides the opcodes and
// the builtin table.

// Serialized artifacts use opcode and builtin positions as IDs. This version
// lets the VM reject an artifact built against a different encoding instead
// of misinterpreting it. It does not require preserving older IDs or artifacts;
// change the version when a format change could alter their meaning.
//
inline constexpr uint32_t VmMbcFormatVersion = 16u;

inline constexpr int VM_MAX_UNION_TAG{ UINT8_MAX };

inline constexpr int VM_MAX_ARRAY_SIZE{ ((UINT16_MAX << 8) | 0xffff) };

// A procedure is named `name@module`, and the runtime splits it to name stack
// frames.
constexpr char VmModuleSeparator = '@';
constexpr std::string_view VM_MAIN_PROCEDURE_PREFIX = "$main$";
constexpr std::string_view VM_MODULE_BOOTSTRAP_PREFIX = "$module_bootstrap$";
