#pragma once

#include <cstdint>
#include <string_view>

// What marmotc writes and marmotvm reads must agree on besides the opcodes and
// the builtin table.

// Bumped 2026-09-12: the 2026-09-12-delete-in-place-mutation plan removed
// six builtins from the MIDDLE of MidoriFFIRegistry's entry table (five in
// Task 6 - ArrayAppend/ArrayPrepend/ArrayExtend/TextAppend/TextPrepend -
// and ArrayPop in Task 9). The bytecode backend serialises a builtin's
// POSITION in that table into the bytecode (BytecodeBackend.cpp), and the
// VM looks the builtin up by that position at call time
// (VirtualMachine.cpp). Removing entries from the middle shifts every
// later entry's position, so a .mmc built against the old table would
// silently call a different builtin with no error. Bumping this forces
// old artifacts to be rejected instead of misexecuted. A future reader
// who removes or reorders a builtin from this table must bump this
// version too - appending new entries at the END does not require it.
//
// Bumped 2026-09-14 (4): 27 opcodes that nothing emitted were deleted from
// the OpCode enum, including SET_ARRAY, the ninth entry. Opcodes are
// serialised by their enum VALUE, so every opcode after the first removed
// one was renumbered. The same rule applies to OpCode as to the FFI table:
// removing or reordering an enum entry requires a bump, appending does not.
//
// Bumped 2026-09-14 (5): JOIN_WORKER gained four operand bytes (the Result
// and WorkerError constructor tags), so an older artifact would be decoded
// with the wrong instruction length. Changing an instruction's length
// requires a bump too.
//
// Bumped 2026-09-16 (8): WORD_TO_TEXT was inserted after INT_TO_TEXT, which
// renumbered every later opcode. `Word as Text` used to lower to
// WORD_TO_INT + INT_TO_TEXT and print values above 2^63 - 1 as negative.
//
// Bumped 2026-09-17 (9): MAKE_CELL, READ_CELL and WRITE_CELL were inserted
// after SET_CELL for Ref<T>, which renumbered every later opcode.
//
// Bumped 2026-09-27 (12): once MidoriIR's backend was the only emitter,
// the 30 opcodes nothing emitted were deleted (the frame-prefix closure
// opcodes, the one-byte procedure, global and text forms, the scope pops
// and others), and the local-operand superinstructions lost the padding
// that let the old code generator rewrite them in place.
//
// Bumped 2026-09-27 (13): CONSTRUCT_UNION carries the tag SET_TAG used to
// set after it, and SET_TAG, which nothing else emitted, was deleted.
// PUSH_PLACEHOLDER carries how many to push.
inline constexpr uint32_t MbcFormatVersion = 13u;

inline constexpr int MAX_UNION_TAG{ UINT8_MAX };

inline constexpr int MAX_ARRAY_SIZE{ ((UINT16_MAX << 8) | 0xffff) };

// A procedure is named `name@module`, and the runtime splits it to name stack
// frames.
constexpr char ModuleSeparator = '@';
constexpr std::string_view MAIN_PROCEDURE_PREFIX = "$main$";
constexpr std::string_view MODULE_BOOTSTRAP_PREFIX = "$module_bootstrap$";
