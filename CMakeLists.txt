cmake_minimum_required(VERSION 3.14)
project(SlothDB VERSION 0.1.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SLOTHDB_BUILD_TESTS "Build tests" ON)
option(SLOTHDB_BUILD_SHELL "Build CLI shell" OFF)
option(SLOTHDB_SANITIZERS "Enable ASan/UBSan" OFF)
# SLOTHDB_EDGE strips Excel, Avro, Arrow IPC, and SQLite readers from the
# build. Target: sub-1MB WASM bundle that fits under Cloudflare Workers'
# 1 MB cap. CSV / Parquet / JSON / HTTP still ship.
option(SLOTHDB_EDGE "Edge/WASM minimal build (excludes Excel/Avro/Arrow/SQLite)" OFF)

include(cmake/CompilerOptions.cmake)
include(cmake/Platform.cmake)

add_subdirectory(src)

if(SLOTHDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
