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
)

add_library(slothdb_lib STATIC ${SLOTHDB_SOURCES})
set_target_properties(slothdb_lib PROPERTIES OUTPUT_NAME "slothdb")

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

# CLI
if(SLOTHDB_BUILD_SHELL)
    add_executable(slothdb ${CMAKE_SOURCE_DIR}/tools/shell/shell.cpp)
    target_link_libraries(slothdb PRIVATE slothdb_lib)
    target_include_directories(slothdb PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()
