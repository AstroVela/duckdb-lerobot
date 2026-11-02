# Integration tests build the Blob-capable fork against our pinned DuckDB host.
include("${CMAKE_CURRENT_LIST_DIR}/../extension_config.cmake")
duckdb_extension_load(lance
    SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../build/lance-source"
    LOAD_TESTS
)
