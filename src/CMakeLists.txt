set(EXTENSION_SOURCES
    lerobot_extension.cpp
    function/lerobot_functions.cpp
    function/scan/lerobot_scan.cpp
    function/video/lerobot_video_frames.cpp
    storage/lerobot_metadata_cache.cpp
)

include_directories(include)

build_static_extension(lerobot ${EXTENSION_SOURCES})
build_loadable_extension(lerobot "" ${EXTENSION_SOURCES})

option(LEROBOT_ENABLE_FFMPEG "Build the native LeRobot FFmpeg video decoder" ON)
if(LEROBOT_ENABLE_FFMPEG)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LEROBOT_FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
            libswscale
        )
    endif()
endif()

if(LEROBOT_ENABLE_FFMPEG AND TARGET PkgConfig::LEROBOT_FFMPEG)
    target_compile_definitions(lerobot_extension PRIVATE LEROBOT_HAVE_FFMPEG=1)
    target_compile_definitions(lerobot_loadable_extension PRIVATE LEROBOT_HAVE_FFMPEG=1)
    target_link_libraries(lerobot_extension PkgConfig::LEROBOT_FFMPEG)
    target_link_libraries(lerobot_loadable_extension PkgConfig::LEROBOT_FFMPEG)
    message(STATUS "LeRobot native FFmpeg decoder enabled")
elseif(LEROBOT_ENABLE_FFMPEG)
    message(WARNING
        "LeRobot native FFmpeg decoder disabled: install the libavformat, "
        "libavcodec, libavutil, and libswscale development packages to enable it"
    )
else()
    message(STATUS "LeRobot native FFmpeg decoder disabled by LEROBOT_ENABLE_FFMPEG=OFF")
endif()

install(
    TARGETS lerobot_extension
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
