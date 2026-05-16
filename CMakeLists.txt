cmake_minimum_required(VERSION 3.14)
project(SlothDB VERSION 0.2.6 LANGUAGES C CXX)

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
# SLOTHDB_ASK_MODEL links llama.cpp so `.ask --model` can run a local
# GGUF model (default target: Qwen2.5-Coder-0.5B-Instruct, Q4_K_M, ~310 MB
# lazy-downloaded on first use). Requires third_party/llama.cpp submodule
# to be initialised. Default OFF — the normal build stays at 8 MB and
# doesn't pull the llama.cpp dependency.
option(SLOTHDB_ASK_MODEL "Embed llama.cpp for .ask --model (local GGUF inference)" OFF)
# SLOTHDB_BUILD_SHARED produces slothdb.dll/.so/.dylib alongside the static
# archive. Required by the Python wheel (ctypes), Node bindings, and any
# other dlopen consumer. Default OFF — the CLI exe links the static lib.
option(SLOTHDB_BUILD_SHARED "Build shared slothdb library (for Python/dlopen consumers)" OFF)

if(SLOTHDB_ASK_MODEL)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
        message(FATAL_ERROR
            "SLOTHDB_ASK_MODEL=ON but third_party/llama.cpp is missing.\n"
            "Run:   git submodule update --init --depth 1 third_party/llama.cpp")
    endif()
    # Disable every llama.cpp sub-target we don't need — we only want the
    # `llama` library target. Set these before add_subdirectory so the
    # cached option defaults don't override us.
    set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_WEBUI    OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
    set(LLAMA_OPENSSL        OFF CACHE BOOL "" FORCE)
    set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
    # MSVC: llama-chat.cpp uses u8"..." literals that become const char8_t*
    # under C++20 and clash with lambdas expecting const char*. This flag
    # reverts u8 literals to const char* without downgrading the parent
    # project's C++20 standard.
    if(MSVC AND TARGET llama)
        target_compile_options(llama PRIVATE /Zc:char8_t-)
    endif()
endif()

include(cmake/CompilerOptions.cmake)
include(cmake/Platform.cmake)

add_subdirectory(src)

if(SLOTHDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
