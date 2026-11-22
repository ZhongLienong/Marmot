cmake_minimum_required(VERSION 3.24)

project(marmotstdlib
    VERSION 1.0.0
    DESCRIPTION "Marmot standard library"
    LANGUAGES CXX
)

file(GLOB_RECURSE MARMOT_STDLIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(MarmotStdLib STATIC ${MARMOT_STDLIB_SOURCES} support/Checksum.cpp support/TextPath.cpp)
target_include_directories(MarmotStdLib PUBLIC src PRIVATE support)
target_compile_features(MarmotStdLib PUBLIC cxx_std_23)
include(FetchContent)
if(MSVC AND NOT DEFINED CMAKE_C_COMPILER AND "$ENV{CC}" STREQUAL "")
    set(CMAKE_C_COMPILER "${CMAKE_CXX_COMPILER}")
endif()
enable_language(C)
FetchContent_Declare(picosha2
    URL https://codeload.github.com/okdshin/PicoSHA2/tar.gz/161cb3fc4170fa7a3eca9e582cebd27cc4d1fe29
    URL_HASH SHA256=6cf473a00c98298d3ddee0aed853e3c799791f49dbc01c996ea46cb248e85802
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(picosha2)
target_link_libraries(MarmotStdLib PRIVATE picosha2)

if(NOT EMSCRIPTEN)
    set(REPROC_INSTALL OFF)
    FetchContent_Declare(reproc
        URL https://codeload.github.com/daandemeyer/reproc/tar.gz/06034a7fca1ec46eddb4997f7764db89380c5216
        URL_HASH SHA256=8644fd3746efb1d88d6b476469002a8533d9a7bb2c377e4ed31d6ca3ad7a216b
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(reproc)
    if(WIN32)
        # reproc 14.2.7 drops empty arguments unless its Windows quoting code surrounds them with quotes.
        get_target_property(reproc_source_dir reproc SOURCE_DIR)
        get_target_property(reproc_sources reproc SOURCES)
        file(READ "${reproc_source_dir}/src/process.windows.c" reproc_windows_source)
        string(REPLACE "bool should_escape = false;" "bool should_escape = argument[0] == 0;"
            reproc_windows_source "${reproc_windows_source}")
        set(reproc_windows_fixed "${CMAKE_CURRENT_BINARY_DIR}/reproc-process.windows.c")
        file(CONFIGURE OUTPUT "${reproc_windows_fixed}" CONTENT "${reproc_windows_source}" @ONLY)
        list(FILTER reproc_sources EXCLUDE REGEX "(^|/)src/process[.]windows[.]c$")
        set_property(TARGET reproc PROPERTY SOURCES "${reproc_sources}")
        target_sources(reproc PRIVATE "${reproc_windows_fixed}")
        target_include_directories(reproc PRIVATE "${reproc_source_dir}/src")
    endif()
    target_link_libraries(MarmotStdLib PRIVATE reproc)
    include(cmake/CompilerWarnings.cmake)
    marmot_compiler_warnings(MarmotStdLib)
endif()
