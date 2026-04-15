file(GLOB_RECURSE SLOTHDB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/catalog/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
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

add_library(slothdb STATIC ${SLOTHDB_SOURCES})

target_include_directories(slothdb PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# CLI Shell
if(SLOTHDB_BUILD_SHELL)
    add_executable(slothdb_shell ${CMAKE_SOURCE_DIR}/tools/shell/shell.cpp)
    target_link_libraries(slothdb_shell PRIVATE slothdb)
    target_include_directories(slothdb_shell PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()
