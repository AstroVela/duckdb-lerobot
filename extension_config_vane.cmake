# Separate entry point: the upstream extension configuration is unchanged.
set(LEROBOT_VANE_DISTRIBUTED ON CACHE BOOL "Build the Vane LeRobot adapter" FORCE)
duckdb_extension_load(parquet)
duckdb_extension_load(json)
duckdb_extension_load(lerobot SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/vane
                      INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/src/include
                      TEST_DIR ${CMAKE_CURRENT_LIST_DIR}/test LOAD_TESTS)
