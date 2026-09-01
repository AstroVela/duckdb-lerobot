cmake_minimum_required(VERSION 3.16)

project(DuckDBLerobotExtension)

# Inherit the standard selected by the embedding DuckDB build. The extension
# source stays within the C++11 language subset, but current official DuckDB
# headers require C++17 and must not be downgraded here.

# This file is evaluated by DuckDB after extension_config.cmake points at this
# repository. Configure from the pinned DuckDB source tree (or use `make`) so
# DuckDB's extension build helpers are available here.
if(NOT COMMAND build_loadable_extension)
    message(FATAL_ERROR
        "Configure with: cmake -S duckdb -B build/release "
        "-DDUCKDB_EXTENSION_CONFIGS=${CMAKE_CURRENT_SOURCE_DIR}/extension_config.cmake"
    )
endif()

add_subdirectory(src)
