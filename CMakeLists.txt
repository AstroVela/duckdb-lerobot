cmake_minimum_required(VERSION 3.16)

project(DuckDBLerobotExtension)

# DuckDB adds an out-of-tree extension as a subdirectory. In that mode its
# build macros are already available, and only the extension sources belong in
# this CMake scope. When this repository is configured directly, the pinned
# DuckDB submodule supplies those macros and the shell target.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUCKDB_EXTENSION_CONFIGS "${CMAKE_CURRENT_SOURCE_DIR}/extension_config.cmake")
    add_subdirectory(duckdb)
else()
    add_subdirectory(src)
endif()
