file(GLOB_RECURSE SLOTHDB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/main/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/parser/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/binder/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/planner/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/execution/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/transaction/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/optimizer/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/api/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extension/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ask/*.cpp"
)

# Edge build drops the readers Cloudflare Workers / browser widgets don't
# need. Every excluded source is also #ifdef'd out of its call sites in
# connection.cpp + physical_planner.cpp, so the final binary has no
# reference to the stripped types.
if(SLOTHDB_EDGE)
    list(FILTER SLOTHDB_SOURCES EXCLUDE REGEX ".*/storage/excel_reader\\.cpp$")
    list(FILTER SLOTHDB_SOURCES EXCLUDE REGEX ".*/storage/avro_reader\\.cpp$")
    list(FILTER SLOTHDB_SOURCES EXCLUDE REGEX ".*/storage/arrow_ipc\\.cpp$")
    list(FILTER SLOTHDB_SOURCES EXCLUDE REGEX ".*/storage/sqlite_scanner\\.cpp$")
    list(FILTER SLOTHDB_SOURCES EXCLUDE REGEX ".*/storage/miniz\\.c$")
endif()

add_library(slothdb_lib STATIC ${SLOTHDB_SOURCES})
set_target_properties(slothdb_lib PROPERTIES OUTPUT_NAME "slothdb")

if(SLOTHDB_EDGE)
    target_compile_definitions(slothdb_lib PUBLIC SLOTHDB_EDGE=1)
endif()

if(SLOTHDB_ASK_MODEL)
    target_compile_definitions(slothdb_lib PUBLIC SLOTHDB_ASK_MODEL=1)
    target_link_libraries(slothdb_lib PRIVATE llama)
    # llama.cpp public headers live at third_party/llama.cpp/include
    target_include_directories(slothdb_lib PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
        ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/ggml/include)
endif()

# miniz is third-party C; suppress warnings-as-errors for it only.
if(MSVC)
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/storage/miniz.c
        PROPERTIES COMPILE_FLAGS "/w"
    )
else()
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/storage/miniz.c
        PROPERTIES COMPILE_FLAGS "-w"
    )
endif()

target_include_directories(slothdb_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# WASM playground build (try.slothdb.org). Produces slothdb.wasm + slothdb.js
# via Emscripten + embind. Single-threaded. No HTTP.
if(EMSCRIPTEN)
    # -Oz optimises for size on edge builds; full builds prefer -O3 speed.
    if(SLOTHDB_EDGE)
        set(SLOTHDB_WASM_OPT "-Oz")
    else()
        set(SLOTHDB_WASM_OPT "-O3")
    endif()

    # Exceptions must be enabled on every translation unit that could throw.
    target_compile_options(slothdb_lib PRIVATE "-fexceptions" ${SLOTHDB_WASM_OPT})
    target_link_options(slothdb_lib INTERFACE "-fexceptions")

    add_executable(slothdb_wasm ${CMAKE_SOURCE_DIR}/tools/playground/wasm_bindings.cpp)
    target_link_libraries(slothdb_wasm PRIVATE slothdb_lib)
    set_target_properties(slothdb_wasm PROPERTIES
        OUTPUT_NAME "slothdb"
        SUFFIX ".js")
    target_compile_options(slothdb_wasm PRIVATE "-fexceptions" ${SLOTHDB_WASM_OPT})

    set(SLOTHDB_WASM_LINK_OPTS
        "-fexceptions"
        "-lembind"
        "-sMODULARIZE=1"
        "-sEXPORT_NAME=createSlothDB"
        "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web,node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=67108864"        # 64 MB
        "-sMAXIMUM_MEMORY=2147483648"      # 2 GB
        "-sFORCE_FILESYSTEM=1"
        "-sEXPORTED_RUNTIME_METHODS=['FS','ccall','cwrap']"
        "-sSTACK_SIZE=5242880"             # 5 MB — parser recursion headroom
        ${SLOTHDB_WASM_OPT}
    )
    if(SLOTHDB_EDGE)
        # Strip every byte we don't need when targeting Workers / edge.
        list(APPEND SLOTHDB_WASM_LINK_OPTS
            "-sMALLOC=emmalloc"           # smaller than dlmalloc
            "-sFILESYSTEM=0"              # override FORCE_FILESYSTEM above
            "-sABORTING_MALLOC=0"
            "-sDISABLE_EXCEPTION_CATCHING=0"
            "--closure=1"                 # minify JS glue
        )
        # Override the FORCE_FILESYSTEM + EXPORTED_RUNTIME that full build sets —
        # edge build reads data via fetch/ArrayBuffer, not emscripten's FS.
        list(REMOVE_ITEM SLOTHDB_WASM_LINK_OPTS
            "-sFORCE_FILESYSTEM=1"
            "-sEXPORTED_RUNTIME_METHODS=['FS','ccall','cwrap']"
        )
        list(APPEND SLOTHDB_WASM_LINK_OPTS
            "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
        )
    endif()
    target_link_options(slothdb_wasm PRIVATE ${SLOTHDB_WASM_LINK_OPTS})
endif()

# CLI
if(SLOTHDB_BUILD_SHELL)
    set(SHELL_SOURCES ${CMAKE_SOURCE_DIR}/tools/shell/shell.cpp)
    # Vendor linenoise for arrow-key line editing, history, UTF-8 handling
    # on POSIX. Windows uses std::getline + cmd.exe's native line editing.
    if(NOT WIN32)
        list(APPEND SHELL_SOURCES ${CMAKE_SOURCE_DIR}/third_party/linenoise/linenoise.c)
    endif()
    add_executable(slothdb ${SHELL_SOURCES})
    target_link_libraries(slothdb PRIVATE slothdb_lib)
    target_include_directories(slothdb PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/third_party/linenoise)
    if(NOT WIN32)
        target_compile_definitions(slothdb PRIVATE SLOTHDB_HAS_LINENOISE=1)
    endif()
endif()
