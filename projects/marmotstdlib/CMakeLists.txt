cmake_minimum_required(VERSION 3.24)

project(marmotstdlib
    VERSION 1.0.0
    DESCRIPTION "Marmot standard library"
    LANGUAGES CXX
)

add_library(MarmotNativeSupport STATIC
    support/Checksum.cpp
    support/TextPath.cpp
)
target_include_directories(MarmotNativeSupport PUBLIC support)
target_compile_features(MarmotNativeSupport PUBLIC cxx_std_23)

file(GLOB_RECURSE MARMOT_STDLIB_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_library(MarmotStdLib STATIC ${MARMOT_STDLIB_SOURCES})
target_include_directories(MarmotStdLib PUBLIC src)
target_compile_features(MarmotStdLib PUBLIC cxx_std_23)
target_link_libraries(MarmotStdLib PUBLIC MarmotNativeSupport)
