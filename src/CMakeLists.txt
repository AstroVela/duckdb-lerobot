set(EXTENSION_SOURCES
    lerobot_extension.cpp
    lerobot_path.cpp
    compat/lerobot_bind_replace.cpp
    function/lerobot_functions.cpp
    function/lerobot_schema.cpp
    function/copy/lerobot_codec_executor.cpp
    function/copy/lerobot_copy.cpp
    function/copy/lerobot_copy_options.cpp
    function/copy/lerobot_video_writer.cpp
    function/scan/lerobot_scan.cpp
    function/temporal/lerobot_temporal_targets.cpp
    function/video/lerobot_video_frames.cpp
    function/video/lerobot_video_options.cpp
    storage/lerobot_metadata_cache.cpp
)

include_directories(include)

build_static_extension(lerobot ${EXTENSION_SOURCES})
build_loadable_extension(lerobot "" ${EXTENSION_SOURCES})

option(LEROBOT_ENABLE_FFMPEG "Build native LeRobot FFmpeg video read/write support" ON)
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
    message(STATUS "LeRobot native FFmpeg video read/write support enabled")
elseif(LEROBOT_ENABLE_FFMPEG)
    message(FATAL_ERROR
        "LEROBOT_ENABLE_FFMPEG=ON requires the libavformat, libavcodec, "
        "libavutil, and libswscale development packages"
    )
else()
    message(STATUS "LeRobot native FFmpeg video read/write support disabled by LEROBOT_ENABLE_FFMPEG=OFF")
endif()

install(
    TARGETS lerobot_extension
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}"
)
