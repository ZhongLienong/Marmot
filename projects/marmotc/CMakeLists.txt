cmake_minimum_required(VERSION 3.24)

# ============================================================================
# Project Configuration
# ============================================================================
project(marmotc
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
        "or build through python scripts/dev.py, which loads that environment.")
endif()

# ============================================================================
# Global Settings
# ============================================================================
# Enforce C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The sources use <print>, <expected> and std::ranges::to. GCC 13 and Clang's
# default libstdc++ on older distributions accept -std=c++23 but lack them, and
# the failure otherwise surfaces as a missing header deep in the build.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <expected>
#include <print>
#include <ranges>
#include <vector>
int main()
{
    std::vector<int> values = std::views::iota(0, 3) | std::ranges::to<std::vector<int>>();
    std::expected<int, int> result = values.front();
    std::print(\"{}\", result.value());
}
" MIDORI_HAS_CXX23_LIBRARY)
if(NOT MIDORI_HAS_CXX23_LIBRARY)
    message(FATAL_ERROR
        "Marmot needs a C++23 standard library with <print>, <expected> and std::ranges::to.\n"
        "Detected compiler: ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})\n"
        "Use MSVC 19.37+, GCC 14+, or Clang 19+ (Clang 18 cannot use libstdc++'s std::expected); "
        "on Linux, for example: CXX=g++-14 cmake --preset linux-dev")
endif()

# Define build configurations (multi-config) or default build type (single-config)
get_property(CMAKE_GENERATOR_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(CMAKE_GENERATOR_MULTI_CONFIG)
    set(CMAKE_CONFIGURATION_TYPES "Debug;RelWithDebInfo;Release" CACHE STRING "" FORCE)
else()
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
endif()

if(NOT CMAKE_GENERATOR_MULTI_CONFIG)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug RelWithDebInfo Release)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo|Release)$")
        message(FATAL_ERROR "Use Debug, RelWithDebInfo (Dev), or Release")
    endif()
endif()

# Release optimization preserves the language's Float semantics.
option(MIDORI_ENABLE_LTO "Enable link-time optimization for Release builds" ON)

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

if(MIDORI_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()

    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.13.0
    )
    FetchContent_MakeAvailable(catch2)
endif()

# Settings every Marmot library and executable shares. Include directories are
# not set here: each component library exports only its own src/ root, so a
# target can include a header only from the components it links (see the
# dependency graph below and scripts/testing/layering.py).
function(midori_configure_target target_name)
    # PUBLIC, so every target that links a Marmot library compiles its headers
    # with the same values as the library. Headers change on them:
    # sizeof(MidoriValue) is 8 without MIDORI_DEBUG_FULL and 16 with it, and a
    # unit test target that once compiled at a different level from the library
    # read past the end of every MidoriValue it passed across. Pinned by
    # tests/runtime/AbiConsistencyTests.cpp.
    target_compile_definitions(${target_name} PUBLIC
        MIDORI_VERSION_STRING="${PROJECT_VERSION}"
        MIDORI_DEBUG_FULL=$<CONFIG:Debug>
        MIDORI_DEBUG_INFO=$<CONFIG:Debug,RelWithDebInfo>
        MIDORI_ENABLE_OPTIMIZER_STATS=$<CONFIG:Debug,RelWithDebInfo>
    )
endfunction()

file(GLOB_RECURSE MARMOT_COMPILER_SOURCES CONFIGURE_DEPENDS "src/Bytecode/*.cpp" "src/Compiler/*.cpp" "src/Utility/*.cpp")
file(GLOB_RECURSE MARMOT_DRIVER_SOURCES CONFIGURE_DEPENDS "src/Utility/CLI/*.cpp" "src/Utility/Driver/*.cpp")
list(FILTER MARMOT_COMPILER_SOURCES EXCLUDE REGEX "/src/Utility/(CLI|Driver)/")
add_library(MarmotCompiler STATIC ${MARMOT_COMPILER_SOURCES})
target_include_directories(MarmotCompiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_library(MarmotDriver STATIC ${MARMOT_DRIVER_SOURCES})
target_link_libraries(MarmotDriver PUBLIC MarmotCompiler)
set(MIDORI_LIBRARY_TARGETS MarmotCompiler MarmotDriver)
foreach(target_name IN LISTS MIDORI_LIBRARY_TARGETS)
    midori_configure_target(${target_name})
endforeach()
if(UNIX)
    target_link_libraries(MarmotCompiler PUBLIC ${CMAKE_DL_LIBS})
endif()
if(NOT EMSCRIPTEN)
    add_executable(marmotc src/Marmot.cpp)
    target_link_libraries(marmotc PRIVATE MarmotDriver)
    set(MIDORI_NATIVE_COMPILE_TARGETS ${MIDORI_LIBRARY_TARGETS} marmotc)
    set(MIDORI_NATIVE_LINK_TARGETS marmotc)
    foreach(target_name IN LISTS MIDORI_NATIVE_COMPILE_TARGETS)
        target_compile_definitions(${target_name} PUBLIC
            $<$<CONFIG:Release>:NDEBUG>
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

            # Debug configuration - best debuggability, no optimization
            $<$<CONFIG:Debug>:/Od>      # No optimization
            $<$<CONFIG:Debug>:/Zi>      # Full debug info

            # RelWithDebInfo configuration - optimized with debug symbols
            $<$<CONFIG:RelWithDebInfo>:/O2>    # Maximize speed
            $<$<CONFIG:RelWithDebInfo>:/Zi>    # Full debug info

            # Release configuration - maximum speed
            $<$<CONFIG:Release>:/O2>    # Maximize speed
            $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>,$<CXX_COMPILER_ID:MSVC>>:/GL>    # MSVC whole program optimization
            $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>    # clang-cl LTO
            $<$<CONFIG:Release>:/Gw>    # Optimize global data
            )
        endforeach()

        # MSVC linker flags
        foreach(target_name IN LISTS MIDORI_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<CONFIG:RelWithDebInfo>:/DEBUG>
                $<$<CONFIG:RelWithDebInfo>:/OPT:REF>
                $<$<CONFIG:RelWithDebInfo>:/OPT:ICF>

                $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>,$<CXX_COMPILER_ID:MSVC>>:/LTCG>
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/RELEASE>
            )
        endforeach()
    endif()

    if(NOT MSVC)
        # GCC's plain -flto runs the link-time step on one thread and warns so.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(MIDORI_LTO_FLAG "-flto=auto")
        else()
            set(MIDORI_LTO_FLAG "-flto")
        endif()

        foreach(target_name IN LISTS MIDORI_NATIVE_COMPILE_TARGETS)
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:RelWithDebInfo>:-O2>
                $<$<CONFIG:RelWithDebInfo>:-g>
                $<$<CONFIG:Release>:-O3>
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>>:${MIDORI_LTO_FLAG}>
            )
        endforeach()

        foreach(target_name IN LISTS MIDORI_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MIDORI_ENABLE_LTO}>>:${MIDORI_LTO_FLAG}>
            )
        endforeach()
    endif()

    install(TARGETS marmotc RUNTIME DESTINATION bin)
else()
    include(cmake/Wasm.cmake)
endif()
if(MIDORI_BUILD_TESTS AND NOT EMSCRIPTEN)
    include(cmake/UnitTests.cmake)
endif()
