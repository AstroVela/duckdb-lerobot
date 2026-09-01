# Build the LeRobot extension from this repository.
duckdb_extension_load(lerobot
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# lerobot_episodes delegates physical Parquet decoding to DuckDB's native
# scanner until the specialised metadata scan is introduced.
duckdb_extension_load(parquet)
