cmake_minimum_required(VERSION 3.16)

project(DuckDBLerobotExtension)

# Inherit the standard selected by the embedding DuckDB build. The extension
# source stays within the C++11 language subset, and the pinned DuckDB 1.5.5
# tree selects C++11 for the complete build.

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

if(BUILD_UNITTESTS AND BUILD_MAIN_DUCKDB_LIBRARY AND NOT PREBUILT_BINARY
   AND NOT DISABLE_THREADS)
    add_subdirectory(test)
endif()

# Keep the loadable artifact at DuckDB's standard extension path even though
# the target itself is organized in the src subdirectory. The distribution
# workflow uploads build/<config>/extension/lerobot/lerobot.duckdb_extension.
set_target_properties(
    lerobot_loadable_extension
    PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
