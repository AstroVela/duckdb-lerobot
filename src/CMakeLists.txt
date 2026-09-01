set(EXTENSION_SOURCES lerobot_extension.cpp)

include_directories(include)

build_static_extension(lerobot ${EXTENSION_SOURCES})
build_loadable_extension(lerobot "" ${EXTENSION_SOURCES})

install(
    TARGETS lerobot_extension
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
