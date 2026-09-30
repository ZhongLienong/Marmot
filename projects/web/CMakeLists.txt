cmake_minimum_required(VERSION 3.24)
project(MarmotWeb VERSION 1.0.0 LANGUAGES CXX)
if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "Configure this project with Emscripten through scripts/dev.py wasm")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set(MIDORI_BUILD_TESTS OFF CACHE BOOL "Build native tests" FORCE)
add_subdirectory(../marmotc marmotc)
add_subdirectory(../marmotvm marmotvm)
set(MIDORI_LIBRARY_TARGETS MarmotCompiler MarmotDriver MarmotRuntime)
option(MIDORI_ENABLE_LTO "Enable link-time optimization" ON)
    message(STATUS "Configuring for WebAssembly build with Emscripten")

    option(MIDORI_WASM64 "Build WebAssembly with memory64 (wasm64)" ON)
    set(MIDORI_WASM64_FLAGS)
    if(MIDORI_WASM64)
        list(APPEND MIDORI_WASM64_FLAGS -sMEMORY64=1)
    endif()

    # Collect WASM-specific files
    file(GLOB_RECURSE WASM_FILES CONFIGURE_DEPENDS
        "src/WASM/*.h"
        "src/WASM/*.cpp"
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
