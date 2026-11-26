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
#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/Support/Expected.h\"
#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/Support/Optional.h\"
#include <print>
#include <ranges>
#include <vector>
int main()
{
    std::vector<int> values = std::views::iota(0, 3) | std::ranges::to<std::vector<int>>();
    MarmotCompiler::Expected<int, int> result = values.front();
    MarmotCompiler::Optional<int> value = result.Value();
    std::print(\"{}\", value.Value());
}
" MARMOT_HAS_CXX23_SUPPORT)
if(NOT MARMOT_HAS_CXX23_SUPPORT)
    message(FATAL_ERROR
        "Marmot needs C++23 explicit object parameters and a standard library with <print>, <expected> and std::ranges::to.\n"
        "Detected compiler: ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})\n"
        "Use MSVC 19.37+, GCC 14+, or Clang 19+; "
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
option(MARMOT_ENABLE_LTO "Enable link-time optimization for Release builds" ON)

set(MARMOT_BUILD_TESTS_DEFAULT ON)
if(EMSCRIPTEN)
    set(MARMOT_BUILD_TESTS_DEFAULT OFF)
elseif(NOT CMAKE_GENERATOR_MULTI_CONFIG)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" MARMOT_BUILD_TYPE_UPPER)
    if(MARMOT_BUILD_TYPE_UPPER STREQUAL "RELEASE")
        set(MARMOT_BUILD_TESTS_DEFAULT OFF)
    endif()
endif()
option(MARMOT_BUILD_TESTS "Build Marmot implementation unit tests" ${MARMOT_BUILD_TESTS_DEFAULT})

if(MSVC)
    # Debug takes the shared runtime (/MDd): each static debug runtime has its own
    # heap, and the VM frees the buffers a native library returns.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:DebugDLL>" CACHE STRING "Use static MSVC runtime (/MT), shared in Debug (/MDd)" FORCE)
endif()

# Output directories for build artifacts
set(MARMOT_OUTPUT_DIR "${CMAKE_BINARY_DIR}/out")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MARMOT_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MARMOT_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${MARMOT_OUTPUT_DIR}")

# ============================================================================
# External Dependencies
# ============================================================================
include(FetchContent)
if(MSVC AND NOT DEFINED CMAKE_C_COMPILER AND "$ENV{CC}" STREQUAL "")
    set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}")
endif()
enable_language(C)

set(YYJSON_INSTALL OFF)
set(ZLIB_BUILD_SHARED OFF)
set(ZLIB_BUILD_TESTING OFF)
set(ZLIB_BUILD_MINIZIP OFF)
set(ZLIB_INSTALL OFF)
FetchContent_Declare(yyjson
    URL https://codeload.github.com/ibireme/yyjson/tar.gz/8b4a38dc994a110abaec8a400615567bd996105f
    URL_HASH SHA256=94e9c90f8f12d8329f2c0756782d21e5c278729023cd2c5fe47e323a35947ed6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(zlib
    URL https://codeload.github.com/madler/zlib/tar.gz/99623ef7097352da7993ba614161fd6a311e046e
    URL_HASH SHA256=f0d590dd90243a12f551ee13359bf6c634249c55b316fa2b7dbecabe7ae9a904
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(yyjson zlib)

if(EMSCRIPTEN)
    target_compile_options(yyjson PRIVATE -sMEMORY64=1)
    target_compile_options(zlibstatic PRIVATE -sMEMORY64=1)
else()
    FetchContent_Declare(CLI11
        URL https://codeload.github.com/CLIUtils/CLI11/tar.gz/37bb6edc5317e99af72ef48405e65d9ca5218861
        URL_HASH SHA256=62ecd3cd7e8ce36506ea8c19e20554a933c82f45d6a75a39cc8778e0c5170a66
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(CLI11)
endif()

if(MARMOT_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()

    FetchContent_Declare(
        catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.13.0
    )
    FetchContent_MakeAvailable(catch2)
endif()

function(marmot_configure_target target_name)
    target_compile_definitions(${target_name} PUBLIC
        MARMOT_VERSION_STRING="${PROJECT_VERSION}"
        MARMOT_DEBUG_INFO=$<CONFIG:Debug,RelWithDebInfo>
        MARMOT_ENABLE_OPTIMIZER_STATS=$<CONFIG:Debug,RelWithDebInfo>
    )
endfunction()

add_library(MarmotSyntax STATIC
    src/Compiler/AbstractSyntaxTree/AbstractSyntaxTree.cpp
    src/Compiler/AbstractSyntaxTree/BindingIdentity.cpp
    src/Compiler/AbstractSyntaxTree/FunctionParameters.cpp
    src/Compiler/AbstractSyntaxTree/GeneratedBinding.cpp
    src/Compiler/AbstractSyntaxTree/Printer/AbstractSyntaxTreePrinter.cpp
    src/Compiler/AbstractSyntaxTree/Type.cpp
    src/Compiler/DerivedDeclarationGenerator/DerivedDeclarationGenerator.cpp
    src/Compiler/Error/CompilerError.cpp
    src/Compiler/Error/CompilerErrorContext.cpp
    src/Compiler/Lexer/Lexer.cpp
    src/Compiler/Module/Module.cpp
    src/Compiler/Module/ModuleImports.cpp
    src/Compiler/Module/ModuleInterface.cpp
    src/Compiler/Module/TypeclassMetadata.cpp
    src/Compiler/Operator/Operator.cpp
    src/Compiler/Operator/Ordering.cpp
    src/Compiler/Parser/ParseDecl.cpp
    src/Compiler/Parser/ParseExpr.cpp
    src/Compiler/Parser/ParseModule.cpp
    src/Compiler/Parser/ParsePattern.cpp
    src/Compiler/Parser/ParseType.cpp
    src/Compiler/Primitive/Primitive.cpp
    src/Compiler/Source/Source.cpp
    src/Compiler/Token/Token.cpp
    src/Compiler/TypeTransform/TypeTransform.cpp
    src/Support/Terminal/Terminal.cpp
)
target_include_directories(MarmotSyntax PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(MarmotSyntax PRIVATE yyjson)

add_library(MarmotFmt STATIC src/Utility/Formatter/Formatter.cpp)
target_link_libraries(MarmotFmt PUBLIC MarmotSyntax)

add_library(MarmotSema STATIC
    src/Compiler/Analysis/AbstractSyntaxTreeWalker.cpp
    src/Compiler/Analysis/DefinitionAnalysis.cpp
    src/Compiler/Analysis/SharedAnalysis.cpp
    src/Compiler/BuildGraph/BuildGraph.cpp
    src/Compiler/CompilationInputs/CompilationInputs.cpp
    src/Compiler/ImportResolver/ImportResolver.cpp
    src/Compiler/ModuleManager/ModuleManager.cpp
    src/Compiler/ModuleManager/ModuleSourceLoader.cpp
    src/Compiler/StaticAnalyzerManager/Diagnostics/BaseDiagnosticPass/BaseDiagnosticPass.cpp
    src/Compiler/StaticAnalyzerManager/Diagnostics/IntegerOverflowDiagnostic/IntegerOverflowDiagnostic.cpp
    src/Compiler/StaticAnalyzerManager/Diagnostics/ShadowingPolicyDiagnostic/ShadowingPolicyDiagnostic.cpp
    src/Compiler/StaticAnalyzerManager/Diagnostics/UnusedLocalDiagnostic/UnusedLocalDiagnostic.cpp
    src/Compiler/StaticAnalyzerManager/StaticAnalyzerManager.cpp
    src/Compiler/TypeChecker/CheckDecl.cpp
    src/Compiler/TypeChecker/CheckExpr.cpp
    src/Compiler/TypeChecker/PatternCoverage.cpp
    src/Compiler/TypeChecker/PatternExhaustiveness.cpp
    src/Compiler/TypeChecker/TraitSolver.cpp
    src/Compiler/TypeMatching/InstanceTypeMatcher.cpp
)
target_link_libraries(MarmotSema PUBLIC MarmotSyntax)
if(MSVC)
    # Recursive type checking of long expressions can exceed the default 1 MiB stack.
    target_link_options(MarmotSema INTERFACE /STACK:4194304)
endif()

add_library(MarmotIR STATIC
    src/Compiler/Lowering/FunctionLowering.cpp
    src/Compiler/Lowering/GenericFunctionTable.cpp
    src/Compiler/Lowering/GenericTypes.cpp
    src/Compiler/Lowering/InstanceResolver.cpp
    src/Compiler/Lowering/Lowering.cpp
    src/Compiler/Lowering/LoweringControl.cpp
    src/Compiler/Lowering/LoweringExpressions.cpp
    src/Compiler/MarmotIR/Builder/MarmotIRBuilder.cpp
    src/Compiler/MarmotIR/MarmotIR.cpp
    src/Compiler/MarmotIR/Printer/MarmotIRPrinter.cpp
    src/Compiler/MarmotIR/Verifier/MarmotIRVerifier.cpp
    src/Compiler/MarmotIROptimizer/Analysis/MarmotIRAnalysis.cpp
    src/Compiler/MarmotIROptimizer/Analysis/MarmotIRModuleFacts.cpp
    src/Compiler/MarmotIROptimizer/Interprocedural/ClosureConversion.cpp
    src/Compiler/MarmotIROptimizer/Interprocedural/Contification.cpp
    src/Compiler/MarmotIROptimizer/Interprocedural/Inlining.cpp
    src/Compiler/MarmotIROptimizer/Interprocedural/MarmotIRBodyCopy.cpp
    src/Compiler/MarmotIROptimizer/MarmotIROptimizer.cpp
    src/Compiler/MarmotIROptimizer/Scalar/DeadCodeElimination.cpp
    src/Compiler/MarmotIROptimizer/Scalar/GlobalValueNumbering.cpp
    src/Compiler/MarmotIROptimizer/Scalar/KnownConstructorThreading.cpp
    src/Compiler/MarmotIROptimizer/Scalar/LoopInvariantCodeMotion.cpp
    src/Compiler/MarmotIROptimizer/Scalar/ParameterUnboxing.cpp
    src/Compiler/MarmotIROptimizer/Scalar/ScalarReplacement.cpp
    src/Compiler/MarmotIROptimizer/Scalar/Sccp.cpp
    src/Compiler/MarmotIROptimizer/Scalar/SelfTailCall.cpp
    src/Compiler/MarmotIROptimizer/Scalar/StrengthReduction.cpp
    src/Utility/Diagnostics/Diagnostics.cpp
)
target_link_libraries(MarmotIR PUBLIC MarmotSema)

add_library(MarmotCodegen STATIC
    src/Bytecode/Artifact/BinaryArtifact.cpp
    src/Bytecode/Executable/Executable.cpp
    src/Compiler/BytecodeBackend/BytecodeBackend.cpp
    src/Compiler/BytecodeLinker/BytecodeLinker.cpp
    src/Compiler/BytecodeModule/BytecodeModule.cpp
    src/Compiler/Module/CompiledModule.cpp
)
target_link_libraries(MarmotCodegen PUBLIC MarmotIR PRIVATE ZLIB::ZLIBSTATIC)

add_library(MarmotCompiler STATIC
    src/Compiler/CompilationSchedule/CompilationExecution.cpp
    src/Compiler/CompilationSchedule/CompilationSchedule.cpp
    src/Compiler/CompilationTimings/CompilationTimings.cpp
    src/Compiler/Compiler.cpp
    src/Compiler/Module/ModuleExports.cpp
    src/Compiler/Module/ModuleInterfaceRegistry.cpp
)
target_link_libraries(MarmotCompiler PUBLIC MarmotCodegen)

set(MARMOT_LIBRARY_TARGETS MarmotSyntax MarmotFmt MarmotSema MarmotIR MarmotCodegen MarmotCompiler)
if(NOT EMSCRIPTEN)
    add_library(MarmotDriver STATIC
        src/Compiler/Json/JsonReader.cpp
        src/Utility/BuildPlan/BuildPlan.cpp
        src/Utility/CLI/CLI.cpp
        src/Utility/Driver/MarmotDriver.cpp
    )
    target_link_libraries(MarmotDriver PUBLIC MarmotCompiler MarmotFmt PRIVATE CLI11::CLI11 yyjson)
    list(APPEND MARMOT_LIBRARY_TARGETS MarmotDriver)
endif()
foreach(target_name IN LISTS MARMOT_LIBRARY_TARGETS)
    marmot_configure_target(${target_name})
endforeach()
if(NOT EMSCRIPTEN)
    add_executable(marmotc src/Marmot.cpp)
    target_link_libraries(marmotc PRIVATE MarmotDriver)
    set(MARMOT_NATIVE_COMPILE_TARGETS ${MARMOT_LIBRARY_TARGETS} marmotc)
    set(MARMOT_NATIVE_LINK_TARGETS marmotc)
    include(cmake/CompilerWarnings.cmake)
    foreach(target_name IN LISTS MARMOT_NATIVE_COMPILE_TARGETS)
        marmot_compiler_warnings(${target_name})
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
        foreach(target_name IN LISTS MARMOT_NATIVE_COMPILE_TARGETS)
            target_compile_options(${target_name} PRIVATE
            /GR-            # Disable RTTI
            /permissive-    # Standards-conformant mode
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
            $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>,$<CXX_COMPILER_ID:MSVC>>:/GL>    # MSVC whole program optimization
            $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>    # clang-cl LTO
            $<$<CONFIG:Release>:/Gw>    # Optimize global data
            )
        endforeach()

        # MSVC linker flags
        foreach(target_name IN LISTS MARMOT_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<CONFIG:RelWithDebInfo>:/DEBUG>
                $<$<CONFIG:RelWithDebInfo>:/OPT:REF>
                $<$<CONFIG:RelWithDebInfo>:/OPT:ICF>

                $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>,$<CXX_COMPILER_ID:MSVC>>:/LTCG>
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>,$<CXX_COMPILER_ID:Clang>>:-flto=thin>
                $<$<CONFIG:Release>:/OPT:REF>
                $<$<CONFIG:Release>:/OPT:ICF>
                $<$<CONFIG:Release>:/RELEASE>
            )
        endforeach()
    endif()

    if(NOT MSVC)
        # GCC's plain -flto runs the link-time step on one thread and warns so.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            set(MARMOT_LTO_FLAG "-flto=auto")
        else()
            set(MARMOT_LTO_FLAG "-flto")
        endif()

        foreach(target_name IN LISTS MARMOT_NATIVE_COMPILE_TARGETS)
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:RelWithDebInfo>:-O2>
                $<$<CONFIG:RelWithDebInfo>:-g>
                $<$<CONFIG:Release>:-O3>
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>>:${MARMOT_LTO_FLAG}>
            )
        endforeach()

        foreach(target_name IN LISTS MARMOT_NATIVE_LINK_TARGETS)
            target_link_options(${target_name} PRIVATE
                $<$<AND:$<CONFIG:Release>,$<BOOL:${MARMOT_ENABLE_LTO}>>:${MARMOT_LTO_FLAG}>
            )
        endforeach()
    endif()

    install(TARGETS marmotc RUNTIME DESTINATION bin)
else()
    include(cmake/Wasm.cmake)
endif()
if(MARMOT_BUILD_TESTS AND NOT EMSCRIPTEN)
    include(cmake/UnitTests.cmake)
endif()
