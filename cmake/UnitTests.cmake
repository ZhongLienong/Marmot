# Each component keeps its unit tests in <component>/tests. They still build as
# one executable; the shared helpers in compiler/tests/support are included as
# "support/...".
file(GLOB_RECURSE MIDORI_UNIT_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/common/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/runtime/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/compiler/tests/*.cpp"
)
list(FILTER MIDORI_UNIT_TEST_SOURCES EXCLUDE REGEX "/compiler/tests/support/")

file(GLOB_RECURSE MIDORI_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/compiler/tests/support/*.cpp"
)

file(GLOB_RECURSE MIDORI_TEST_SUPPORT_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/compiler/tests/support/*.h"
)

add_library(MidoriTestSupport STATIC
    ${MIDORI_TEST_SUPPORT_SOURCES}
    ${MIDORI_TEST_SUPPORT_HEADERS}
)

target_include_directories(MidoriTestSupport PUBLIC
    ${CMAKE_SOURCE_DIR}/compiler/tests
)

target_link_libraries(MidoriTestSupport PUBLIC
    MidoriCore
)

add_executable(MarmotUnitTests
    ${MIDORI_UNIT_TEST_SOURCES}
)

target_include_directories(MarmotUnitTests PRIVATE
    ${CMAKE_SOURCE_DIR}/compiler/tests
)

# No build-configuration defines here on purpose. MidoriCore declares
# MIDORI_BUILD_*, NDEBUG, the endian macros and MIDORI_VERSION_STRING as PUBLIC,
# so both test targets inherit them through the link to MidoriCore and see
# BuildConfig.h exactly as the library does. Copying them here instead is what
# let them drift before; common/tests/common/AbiConsistencyTests.cpp fails if a
# target ever compiles those headers differently from MidoriCore again.

target_link_libraries(MarmotUnitTests PRIVATE
    Catch2::Catch2WithMain
    MidoriTestSupport
)

if(MSVC)
    target_compile_options(MidoriTestSupport PRIVATE /EHsc)
    target_compile_options(MarmotUnitTests PRIVATE /EHsc)
endif()

include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(MarmotUnitTests)
