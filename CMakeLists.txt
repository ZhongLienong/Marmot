cmake_minimum_required(VERSION 3.24)

# ============================================================================
# Project Configuration
# ============================================================================
project(Marmot
    VERSION 1.0.0
    DESCRIPTION "Marmot Programming Language"
    LANGUAGES CXX
)

# ============================================================================
# Architecture Guard
# ============================================================================
if(NOT EMSCRIPTEN AND CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "Marmot requires a 64-bit toolchain (sizeof(void*) == 8).\n"
        "Detected compiler: ${CMAKE_CXX_COMPILER}\n"
        "On Windows with Ninja, run from a 'x64 Native Tools Command Prompt' "
        "or use vcvarsall.bat x64 before configuring.")
endif()

# ============================================================================
# Global Settings
# ============================================================================
# Enforce C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Define build configurations (multi-config) or default build type (single-config)
if(CMAKE_GENERATOR_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Development;Release;Experimental" CACHE STRING "" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()

# Performance tuning options (Experimental is an aggressive, non-portable speed profile)
option(MIDORI_ENABLE_LTO "Enable link-time optimization for Release builds" ON)
option(MIDORI_PROFILE_OPCODES "Count dynamic opcode pairs in the VM and dump them at HALT" OFF)

set(MIDORI_BUILD_TESTS_DEFAULT ON)
if(EMSCRIPTEN)
    set(MIDORI_BUILD_TESTS_DEFAULT OFF)
elseif(NOT CMAKE_GENERATOR_MULTI_CONFIG)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" MIDORI_BUILD_TYPE_UPPER)
    if(MIDORI_BUILD_TYPE_UPPER STREQUAL "RELEASE")
        set(MIDORI_BUILD_TESTS_DEFAULT OFF)
    endif()
endif()
option(MIDORI_BUILD_TESTS "Build Marmot implementation unit tests" ${MIDORI_BUILD_TESTS_DEFAULT})

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "Use static MSVC runtime (/MT, /MTd in Debug)" FORCE)
endif()

# Output directories for build artifacts
set(MIDORI_OUTPUT_DIR "${CMAKE_BINARY_DIR}/out")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MIDORI_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MIDORI_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${MIDORI_OUTPUT_DIR}")

# ============================================================================
# External Dependencies
# ============================================================================
include(FetchContent)

FetchContent_Declare(
    toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG v4.2.0
)
FetchContent_MakeAvailable(toml11)

if(MIDORI_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()

    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.13.0
    )
    FetchContent_MakeAvailable(catch2)
endif()

# ============================================================================
# Platform Detection
# ============================================================================
# Test system endianness
include(TestBigEndian)
test_big_endian(IS_BIG_ENDIAN)

set(MIDORI_ENDIAN_DEFINES
    $<$<BOOL:${IS_BIG_ENDIAN}>:MIDORI_BIG_ENDIAN>
    $<IF:$<BOOL:${IS_BIG_ENDIAN}>,,MIDORI_LITTLE_ENDIAN>
)

# Settings every Marmot library and executable shares. Include directories are
# not set here: each component library exports only its own src/ root, so a
# target can include a header only from the components it links (see the
# dependency graph below and scripts/check_layering.py).
function(midori_configure_target target_name)
    # PUBLIC because headers read them: BuildConfig.h defines an inline constexpr
    # VersionString from MIDORI_VERSION_STRING, and falls back to its own endian
    # detection when neither endian macro is set. Every translation unit that
    # includes those headers has to see the same values as the library it links,
    # or the program has two different definitions of the same inline entity.
    target_compile_definitions(${target_name} PUBLIC
        ${MIDORI_ENDIAN_DEFINES}
        MIDORI_VERSION_STRING="${PROJECT_VERSION}"
    )
    target_compile_definitions(${target_name} PRIVATE
        $<$<BOOL:${MIDORI_PROFILE_OPCODES}>:MIDORI_PROFILE_OPCODES=1>
    )
endfunction()

# ============================================================================
# Component Libraries
# ============================================================================
# One library per component, linked in one direction only:
#
#   MarmotCommon    common/src     values, errors, the bytecode format, the
#                                  builtin table's names and signatures
#   MarmotRuntime   runtime/src    the VM and builtin library     -> Common
#   MarmotCompiler  compiler/src   source to bytecode             -> Common
#   MarmotDriver    compiler/src/Utility/{CLI,Driver,TestRunner,OutputCapture}
#                                  the application layer          -> Compiler, Runtime
#
# The compiler never links the runtime. The entry points (compiler/src/Marmot.cpp,
# web/src) are added per build below.
set(MIDORI_DRIVER_DIRS CLI Driver TestRunner OutputCapture)

file(GLOB_RECURSE MARMOT_COMMON_SOURCES CONFIGURE_DEPENDS
    "common/src/*.h"
    "common/src/*.def"
    "common/src/*.cpp"
)

file(GLOB_RECURSE MARMOT_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "runtime/src/*.h"
    "runtime/src/*.cpp"
)

file(GLOB_RECURSE MARMOT_COMPILER_SOURCES CONFIGURE_DEPENDS
    "compiler/src/Compiler/*.h"
    "compiler/src/Compiler/*.cpp"
    "compiler/src/Utility/*.h"
    "compiler/src/Utility/*.cpp"
)

set(MARMOT_DRIVER_SOURCES)
foreach(driver_dir IN LISTS MIDORI_DRIVER_DIRS)
    file(GLOB_RECURSE driver_dir_sources CONFIGURE_DEPENDS
        "compiler/src/Utility/${driver_dir}/*.h"
        "compiler/src/Utility/${driver_dir}/*.cpp"
    )
    list(APPEND MARMOT_DRIVER_SOURCES ${driver_dir_sources})
    list(FILTER MARMOT_COMPILER_SOURCES EXCLUDE REGEX "/compiler/src/Utility/${driver_dir}/")
endforeach()

add_library(MarmotCommon STATIC ${MARMOT_COMMON_SOURCES})
target_include_directories(MarmotCommon PUBLIC ${CMAKE_SOURCE_DIR}/common/src)

add_library(MarmotRuntime STATIC ${MARMOT_RUNTIME_SOURCES})
target_include_directories(MarmotRuntime PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
target_link_libraries(MarmotRuntime PUBLIC MarmotCommon)

add_library(MarmotCompiler STATIC ${MARMOT_COMPILER_SOURCES})
target_include_directories(MarmotCompiler PUBLIC ${CMAKE_SOURCE_DIR}/compiler/src)
target_link_libraries(MarmotCompiler PUBLIC MarmotCommon)
target_link_libraries(MarmotCompiler PRIVATE toml11::toml11)

add_library(MarmotDriver STATIC ${MARMOT_DRIVER_SOURCES})
target_link_libraries(MarmotDriver PUBLIC MarmotCompiler MarmotRuntime)

set(MIDORI_LIBRARY_TARGETS
    MarmotCommon
    MarmotRuntime
    MarmotCompiler
    MarmotDriver
)
foreach(target_name IN LISTS MIDORI_LIBRARY_TARGETS)
    midori_configure_target(${target_name})
endforeach()

# The interpreter dispatch loop is compiled without whole-program optimization:
# its hot dependencies are all force-inlined from headers, and opting out of
# LTCG keeps its codegen stable instead of shifting with unrelated changes in
# other translation units.
if(MSVC)
    set_source_files_properties(runtime/src/Interpreter/VirtualMachine/VirtualMachine.cpp
        PROPERTIES COMPILE_OPTIONS "/GL-")
endif()

if(UNIX)
    target_link_libraries(MarmotCommon PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(MarmotCompiler PUBLIC ${CMAKE_DL_LIBS})
endif()

# ============================================================================
# WebAssembly Build Configuration
# ============================================================================
if(EMSCRIPTEN)
    message(STATUS "Configuring for WebAssembly build with Emscripten")

    option(MIDORI_WASM64 "Build WebAssembly with memory64 (wasm64)" ON)
    set(MIDORI_WASM64_FLAGS)
    if(MIDORI_WASM64)
        list(APPEND MIDORI_WASM64_FLAGS -sMEMORY64=1)
    endif()

    # Collect WASM-specific files
    file(GLOB_RECURSE WASM_FILES CONFIGURE_DEPENDS
        "web/src/WASM/*.h"
        "web/src/WASM/*.cpp"
    )

    # Create WASM executable (no main() needed)
    add_executable(Marmot
        ${WASM_FILES}
    )
    target_link_libraries(Marmot PRIVATE MarmotCompiler MarmotRuntime)

    set(MIDORI_WASM_COMPILE_TARGETS
        ${MIDORI_LIBRARY_TARGETS}
        Marmot
    )

    foreach(target_name IN LISTS MIDORI_WASM_COMPILE_TARGETS)
        target_compile_definitions(${target_name} PRIVATE MIDORI_WASM=1)
        if(MIDORI_WASM64)
            target_compile_definitions(${target_name} PRIVATE MIDORI_WASM64=1)
        endif()
    endforeach()

    # Target-specific compile options
    foreach(target_name IN LISTS MIDORI_WASM_COMPILE_TARGETS)
        target_compile_options(${target_name} PRIVATE
            -fexceptions
            -O3
            -Wno-deprecated-literal-operator
            $<$<BOOL:${MIDORI_ENABLE_LTO}>:-flto>
            ${MIDORI_WASM64_FLAGS}
        )
    endforeach()

    # Emscripten linker options
    target_link_options(Marmot PRIVATE
        --bind
        -sALLOW_MEMORY_GROWTH=1
        -sMODULARIZE=1
        -sEXPORT_NAME=createMarmotModule
        -sENVIRONMENT=web
        -sSTACK_SIZE=5MB
        -sINITIAL_MEMORY=32MB
        -sMAXIMUM_MEMORY=2GB
        -sALLOW_TABLE_GROWTH=1
        -sNO_DISABLE_EXCEPTION_CATCHING
        -sFORCE_FILESYSTEM=1
        -sEXPORTED_RUNTIME_METHODS=FS,FS_createPath,FS_createDataFile,wasmTable
        -sEXPORTED_FUNCTIONS=_malloc,_free
        $<$<BOOL:${MIDORI_ENABLE_LTO}>:-flto>
        ${MIDORI_WASM64_FLAGS}
        --no-entry
    )

    # Set output properties
    set_target_properties(Marmot PROPERTIES
        OUTPUT_NAME "marmot"
        SUFFIX ".js"
    )

# ============================================================================
# Native Build Configuration (Windows/Linux/macOS)
# ============================================================================
else()
    message(STATUS "Configuring for native build")

    # marmotc, the compiler: the thin CLI executable over the driver. The
    # `marmot` project tool (tool/) runs it.
    add_executable(marmotc
        "compiler/src/Marmot.cpp"
    )
    target_link_libraries(marmotc PRIVATE MarmotDriver)
    set(MIDORI_NATIVE_COMPILE_TARGETS
        ${MIDORI_LIBRARY_TARGETS}
        marmotc
    )
    set(MIDORI_NATIVE_LINK_TARGETS marmotc)

    # Detect AVX2 compiler flag for Experimental configuration
    set(MIDORI_HAS_AVX2_FLAG FALSE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(MIDORI_AVX2_FLAG "-mavx2")
    elseif(MSVC)
        set(MIDORI_AVX2_FLAG "/arch:AVX2")
    else()
        set(MIDORI_AVX2_FLAG "-mavx2")
    endif()
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("${MIDORI_AVX2_FLAG}" MIDORI_HAS_AVX2_FLAG)

    set(MIDORI_HAS_AVX2_CONFIG FALSE)
    if(CMAKE_GENERATOR_MULTI_CONFIG)
        list(FIND CMAKE_CONFIGURATION_TYPES "Experimental" MIDORI_AVX2_CONFIG_INDEX)
        if(NOT MIDORI_AVX2_CONFIG_INDEX EQUAL -1)
            set(MIDORI_HAS_AVX2_CONFIG TRUE)
        endif()
    else()
        if(CMAKE_BUILD_TYPE STREQUAL "Experimental")
            set(MIDORI_HAS_AVX2_CONFIG TRUE)
        endif()
    endif()
    if(MIDORI_HAS_AVX2_CONFIG AND NOT MIDORI_HAS_AVX2_FLAG)
        message(WARNING "Experimental config requested but compiler does not support ${MIDORI_AVX2_FLAG}. Building without AVX2.")
    endif()

    # PUBLIC, so every target that links a Marmot library inherits them. They select
    # MIDORI_DEBUG_LEVEL in BuildConfig.h, which changes type layouts in headers
    # (sizeof(MidoriValue) is 8 at levels 1-2 and 16 at level 3). Before these were
    # PUBLIC, the unit test targets compiled those headers at a different level
    # from the library, which in x64-debug was a live out-of-bounds read. Pinned by
    # common/tests/common/AbiConsistencyTests.cpp.
    foreach(target_name IN LISTS MIDORI_NATIVE_COMPILE_TARGETS)
        target_compile_definitions(${target_name} PUBLIC
            $<$<CONFIG:Debug>:MIDORI_BUILD_DEBUG>
            $<$<CONFIG:Development>:MIDORI_BUILD_DEVELOPMENT>
            $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:NDEBUG>
        )
    endforeach()

    # ========================================================================
    # MSVC-Specific Configuration
    # ========================================================================
    if(MSVC)
        # Remove default conflicting flags
        foreach(flag_var
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG
            CMAKE_CXX_FLAGS_RELEASE CMAKE_CXX_FLAGS_MINSIZEREL
            CMAKE_CXX_FLAGS_RELWITHDEBINFO)
            string(REPLACE "/GR" "" ${flag_var} "${${flag_var}}")
            string(REPLACE "/W3" "" ${flag_var} "${${flag_var}}")
            string(REPLACE "/EHsc" "" ${flag_var} "${${flag_var}}")
            string(REPLACE "/RTC1" "" ${flag_var} "${${flag_var}}")
        endforeach()

        # Common MSVC flags for Marmot executable
        foreach(target_name IN LISTS MIDORI_NATIVE_COMPILE_TARGETS)
            target_compile_options(${target_name} PRIVATE
            /GR-            # Disable RTTI
            /permissive-    # Standards-conformant mode
            /W4             # Warning level 4
            /EHsc           # Enable exception handling
            /bigobj         # Allow large object files
            /wd5311         # Suppress deprecated string-literal operator warning (from toml11)

            # Debug configuration - best debuggability, no optimization
            $<$<CONFIG:Debug>:/Od>      # No optimization
            $<$<CONFIG:Debug>:/Zi>      # Full debug info

            # Development configuration - optimized with debug symbols
            $<$<CONFIG:Development>:/O2>    # Maximize speed
            $<$<CONFIG:Development>:/Zi>    # Full debug info

            # Release/Experimental configuration - maximum speed
            $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:/O2>    # Maximize speed
            $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>,$<BOOL:${MIDORI_ENABLE_LTO}>>:/GL>    # Whole program optimization (LTCG)
            $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:/Gw>    # Optimize global data
            $<$<AND:$<CONFIG:Experimental>,$<BOOL:${MIDORI_HAS_AVX2_FLAG}>>:${MIDORI_AVX2_FLAG}>
            $<$<CONFIG:Experimental>:/Gy>   # Function-level linking
            $<$<CONFIG:Experimental>:/GF>   # String pooling
            $<$<CONFIG:Experimental>:/fp:fast>   # Fast-math
            )
        endforeach()

        # MSVC linker flags
        foreach(target_name IN LISTS MIDORI_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<CONFIG:Development>:/DEBUG>
                $<$<CONFIG:Development>:/OPT:REF>
                $<$<CONFIG:Development>:/OPT:ICF>

                $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>,$<BOOL:${MIDORI_ENABLE_LTO}>>:/LTCG>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:/OPT:REF>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:/OPT:ICF>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:/RELEASE>
            )
        endforeach()
    endif()

    if(NOT MSVC)
        foreach(target_name IN LISTS MIDORI_NATIVE_COMPILE_TARGETS)
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Development>:-O2>
                $<$<CONFIG:Development>:-g>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>:-O3>
                $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>,$<BOOL:${MIDORI_ENABLE_LTO}>>:-flto>
                $<$<AND:$<CONFIG:Experimental>,$<BOOL:${MIDORI_HAS_AVX2_FLAG}>>:${MIDORI_AVX2_FLAG}>
                $<$<CONFIG:Experimental>:-fomit-frame-pointer>
                $<$<CONFIG:Experimental>:-funroll-loops>
                $<$<CONFIG:Experimental>:-ffast-math>
            )
        endforeach()

        foreach(target_name IN LISTS MIDORI_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<AND:$<OR:$<CONFIG:Release>,$<CONFIG:Experimental>>,$<BOOL:${MIDORI_ENABLE_LTO}>>:-flto>
            )
        endforeach()
    endif()

endif()

if(MIDORI_BUILD_TESTS AND NOT EMSCRIPTEN)
    include(cmake/UnitTests.cmake)
endif()
