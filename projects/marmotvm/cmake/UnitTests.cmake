file(GLOB_RECURSE UNIT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
list(FILTER UNIT_SOURCES EXCLUDE REGEX "/tests/support/")
file(GLOB_RECURSE SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp")

add_executable(MarmotvmUnitTests ${UNIT_SOURCES} ${SUPPORT_SOURCES})
target_include_directories(MarmotvmUnitTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(MarmotvmUnitTests PRIVATE Catch2::Catch2WithMain MarmotRuntime )
if(MSVC)
    target_compile_options(MarmotvmUnitTests PRIVATE /EHsc)
endif()
include(${catch2_SOURCE_DIR}/extras/Catch.cmake)
catch_discover_tests(MarmotvmUnitTests)

target_compile_definitions(MarmotvmUnitTests PRIVATE MARMOT_MMC_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../../format/mmc/fixtures")
