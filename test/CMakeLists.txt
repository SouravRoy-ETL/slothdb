file(GLOB_RECURSE TEST_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
)

add_executable(slothdb_tests ${TEST_SOURCES})

target_link_libraries(slothdb_tests PRIVATE slothdb_lib)

target_include_directories(slothdb_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/doctest
)

add_test(NAME slothdb_tests COMMAND slothdb_tests)
