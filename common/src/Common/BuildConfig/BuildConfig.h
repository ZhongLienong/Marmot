#pragma once

#include <cstdint>
#include <cstdlib>
#include <string_view>

#ifndef MIDORI_VERSION_STRING
    #define MIDORI_VERSION_STRING "1.0.0"
#endif

// Cross-compiler force inline macro for hot path functions
#if defined(_MSC_VER)
    #define MIDORI_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define MIDORI_FORCE_INLINE __attribute__((always_inline)) inline
#else
    #define MIDORI_FORCE_INLINE inline
#endif

// Cross-compiler no-inline macro for cold slow paths that must not bloat hot callers
#if defined(_MSC_VER)
    #define MIDORI_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    #define MIDORI_NOINLINE __attribute__((noinline))
#else
    #define MIDORI_NOINLINE
#endif

// Compiler hint for unreachable code
#if defined(_MSC_VER)
    #define MIDORI_UNREACHABLE() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
    #define MIDORI_UNREACHABLE() __builtin_unreachable()
#else
    #define MIDORI_UNREACHABLE() ((void)0)
#endif

// Endianness detection (fallback if not defined by CMake)
#if !defined(MIDORI_LITTLE_ENDIAN) && !defined(MIDORI_BIG_ENDIAN)
    #if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define MIDORI_BIG_ENDIAN
    #elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define MIDORI_LITTLE_ENDIAN
    #elif defined(_WIN32) || defined(__x86_64__) || defined(__i386__)
        #define MIDORI_LITTLE_ENDIAN
    #else
        #error "Cannot determine endianness"
    #endif
#endif

// Build configuration levels for Marmot compiler
//
// MIDORI_BUILD_DEBUG (3):
//   - Full debugging output
//   - AST dumps
//   - Bytecode disassembly
//   - Runtime stack traces
//   - Detailed compilation info
//   - Optimizer statistics
//
// MIDORI_BUILD_DEVELOPMENT (2):
//   - Compilation progress and details
//   - Optimizer statistics
//   - Linker information
//   - NO AST dumps
//   - NO bytecode disassembly
//   - NO runtime stack traces
//
// MIDORI_BUILD_RELEASE (1):
//   - Standard compiler messages only
//   - Error and warning output
//   - Minimal runtime info

// Define build level based on CMake configuration
#if defined(MIDORI_BUILD_DEBUG)
    #define MIDORI_DEBUG_LEVEL 3
#elif defined(MIDORI_BUILD_DEVELOPMENT)
    #define MIDORI_DEBUG_LEVEL 2
#else
    #define MIDORI_DEBUG_LEVEL 1
    #define MIDORI_BUILD_RELEASE
#endif

// Convenience macros for conditional compilation
#define MIDORI_DEBUG_FULL (MIDORI_DEBUG_LEVEL >= 3)
#define MIDORI_DEBUG_INFO (MIDORI_DEBUG_LEVEL >= 2)
#define MIDORI_DEBUG_MINIMAL (MIDORI_DEBUG_LEVEL >= 1)

namespace MidoriBuild
{
    inline constexpr std::string_view VersionString = MIDORI_VERSION_STRING;

    // Bumped 2026-09-12: the 2026-09-12-delete-in-place-mutation plan removed
    // six builtins from the MIDDLE of MidoriFFIRegistry's entry table (five in
    // Task 6 - ArrayAppend/ArrayPrepend/ArrayExtend/TextAppend/TextPrepend -
    // and ArrayPop in Task 9). The code generator serialises a builtin's
    // POSITION in that table into the bytecode (CodeGenerator.cpp), and the
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
    // after SET_CELL for Cell<T>, which renumbered every later opcode.
    inline constexpr uint32_t MbcFormatVersion = 9u;

    [[nodiscard]] inline bool EnvironmentFlagEnabledUncached(const char* name) noexcept
    {
#ifdef _WIN32
        char* value = nullptr;
        size_t length = 0u;
        if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        {
            return false;
        }
#else
        const char* value = std::getenv(name);
        if (value == nullptr)
        {
            return false;
        }
#endif

        const std::string_view view(value);
        const bool enabled = !view.empty() && view != "0" && view != "false" && view != "False" && view != "FALSE";

#ifdef _WIN32
        free(value);
#endif

        return enabled;
    }

    class ScopedTestModeOverride
    {
    private:
        int m_previous_value = -1;

    public:
        explicit ScopedTestModeOverride(bool enabled) noexcept;

        ~ScopedTestModeOverride() noexcept;

        ScopedTestModeOverride(const ScopedTestModeOverride&) = delete;
        ScopedTestModeOverride& operator=(const ScopedTestModeOverride&) = delete;
        ScopedTestModeOverride(ScopedTestModeOverride&&) = delete;
        ScopedTestModeOverride& operator=(ScopedTestModeOverride&&) = delete;
    };

    [[nodiscard]] bool IsTestMode() noexcept;

    [[nodiscard]] bool ShouldEmitInternalDiagnostics() noexcept;

    // The configuration MidoriCore itself was compiled with. Defined out of line
    // in BuildConfig.cpp on purpose: a translation unit compares these against
    // its OWN MIDORI_DEBUG_LEVEL, MIDORI_VERSION_STRING and endianness to detect
    // that it read this header differently from the library it links. An inline
    // definition could not detect that, because the linker keeps one copy of an
    // inline function and both sides would see the same answer.
    [[nodiscard]] int LibraryDebugLevel() noexcept;

    [[nodiscard]] std::string_view LibraryVersionString() noexcept;

    [[nodiscard]] bool LibraryIsLittleEndian() noexcept;
}

// Feature flags based on build level
#if (MIDORI_DEBUG_LEVEL >= 3)
    #define MIDORI_ENABLE_AST_DUMP 1
    #define MIDORI_ENABLE_DISASSEMBLY 1
    #define MIDORI_ENABLE_EXECUTION_TRACE 1
    #define MIDORI_ENABLE_COMPILATION_INFO 1
    #define MIDORI_ENABLE_OPTIMIZER_STATS 1
    #define MIDORI_ENABLE_LINKER_INFO 1
    #define MIDORI_ENABLE_MULTITHREADING 0
#elif (MIDORI_DEBUG_LEVEL >= 2)
    #define MIDORI_ENABLE_AST_DUMP 0
    #define MIDORI_ENABLE_DISASSEMBLY 1
    #define MIDORI_ENABLE_EXECUTION_TRACE 0
    #define MIDORI_ENABLE_COMPILATION_INFO 1
    #define MIDORI_ENABLE_OPTIMIZER_STATS 1
    #define MIDORI_ENABLE_LINKER_INFO 1
    #define MIDORI_ENABLE_MULTITHREADING 0
#else
    #define MIDORI_ENABLE_AST_DUMP 0
    #define MIDORI_ENABLE_DISASSEMBLY 0
    #define MIDORI_ENABLE_EXECUTION_TRACE 0
    #define MIDORI_ENABLE_COMPILATION_INFO 0
    #define MIDORI_ENABLE_OPTIMIZER_STATS 0
    #define MIDORI_ENABLE_LINKER_INFO 0
    #define MIDORI_ENABLE_MULTITHREADING 1
#endif
