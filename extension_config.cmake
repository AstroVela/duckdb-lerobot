# LeRobot scan functions copy the native JSON and Parquet function sets at
# extension load time, so register those dependencies first for linked builds.
duckdb_extension_load(parquet)
duckdb_extension_load(json)

if (DONT_LINK OR "$ENV{DONT_LINK}")
    set(LEROBOT_DONT_LINK "DONT_LINK")
else()
    set(LEROBOT_DONT_LINK "")
endif()

# Build the LeRobot extension from this repository.
duckdb_extension_load(lerobot
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    ${LEROBOT_DONT_LINK}
)
