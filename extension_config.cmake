# Build the LeRobot extension from this repository.
duckdb_extension_load(lerobot
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
