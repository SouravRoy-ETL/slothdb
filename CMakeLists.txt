cmake_minimum_required(VERSION 3.14)
project(SlothDB VERSION 0.1.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLOTHDB_BUILD_TESTS "Build tests" ON)
option(SLOTHDB_BUILD_SHELL "Build CLI shell" OFF)
option(SLOTHDB_SANITIZERS "Enable ASan/UBSan" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/Platform.cmake)

add_subdirectory(src)

if(SLOTHDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
