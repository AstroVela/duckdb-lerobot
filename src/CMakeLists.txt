set(EXTENSION_SOURCES
    lerobot_extension.cpp
    lerobot_path.cpp
    lerobot_query.cpp
    compat/lerobot_bind_replace.cpp
    function/lerobot_functions.cpp
    function/lerobot_schema.cpp
    function/image/lerobot_decode_image.cpp
    function/copy/lerobot_codec_executor.cpp
    function/copy/lerobot_copy.cpp
    function/copy/lerobot_copy_options.cpp
    function/copy/lerobot_numeric_stats.cpp
    function/copy/lerobot_video_io.cpp
    function/copy/lerobot_video_writer.cpp
    function/scan/lerobot_scan.cpp
    function/temporal/lerobot_temporal_targets.cpp
    function/video/lerobot_video_frames.cpp
    function/video/lerobot_video_options.cpp
    storage/lerobot_metadata_cache.cpp
    storage/lerobot_timestamp_lookup.cpp)

include_directories(include)

build_static_extension(lerobot ${EXTENSION_SOURCES})
build_loadable_extension(lerobot "" ${EXTENSION_SOURCES})

option(LEROBOT_ENABLE_FFMPEG
       "Build native LeRobot FFmpeg video read/write support" ON)
set(LEROBOT_AV1_ENCODER
    "auto"
    CACHE STRING "AV1 encoder backend: auto, svt, or libaom")
set_property(CACHE LEROBOT_AV1_ENCODER PROPERTY STRINGS auto svt libaom)

if(NOT LEROBOT_AV1_ENCODER MATCHES "^(auto|svt|libaom)$")
  message(FATAL_ERROR "LEROBOT_AV1_ENCODER must be auto, svt, or libaom")
endif()

# Keep aggregate results separate from pkg_check_modules' cached output.
set(LEROBOT_MEDIA_LIBRARIES "")
set(LEROBOT_MEDIA_INCLUDE_DIRS "")
if(LEROBOT_ENABLE_FFMPEG)
  if(VCPKG_BUILD)
    # The vcpkg FFmpeg port supplies FindFFMPEG.cmake and all transitive static
    # link dependencies. Distribution builds must not silently degrade to
    # metadata-only binaries.
    #
    # Its discovery result is cached, but changing manifest features can replace
    # FFmpeg and its transitive libraries during this same configure. Refresh
    # the aggregate result so, for example, zlib is added and x265 is removed
    # when the corresponding feature set changes.
    foreach(LEROBOT_FFMPEG_CACHE_VAR
            FFMPEG_FOUND FFMPEG_LIBRARIES FFMPEG_LIBRARY FFMPEG_DEPS_LIBRARY
            FFMPEG_DEPS_LIBRARY_RELEASE FFMPEG_DEPS_LIBRARY_DEBUG)
      unset(${LEROBOT_FFMPEG_CACHE_VAR})
      unset(${LEROBOT_FFMPEG_CACHE_VAR} CACHE)
    endforeach()
    find_package(FFMPEG REQUIRED)
    set(LEROBOT_MEDIA_LIBRARIES ${FFMPEG_LIBRARIES})
    set(LEROBOT_MEDIA_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIRS})
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(
        LEROBOT_FFMPEG
        QUIET
        IMPORTED_TARGET
        GLOBAL
        libavformat
        libavcodec
        libavutil
        libswscale)
      if(TARGET PkgConfig::LEROBOT_FFMPEG)
        set(LEROBOT_MEDIA_LIBRARIES PkgConfig::LEROBOT_FFMPEG)
      endif()
    endif()
  endif()
endif()

set(LEROBOT_MEDIA_LIBRARIES ${LEROBOT_MEDIA_LIBRARIES} PARENT_SCOPE)
set(LEROBOT_MEDIA_INCLUDE_DIRS ${LEROBOT_MEDIA_INCLUDE_DIRS} PARENT_SCOPE)

if(LEROBOT_MEDIA_LIBRARIES)
  foreach(LEROBOT_TARGET lerobot_extension lerobot_loadable_extension)
    target_compile_definitions(${LEROBOT_TARGET} PRIVATE LEROBOT_HAVE_FFMPEG=1)
    if(LEROBOT_MEDIA_INCLUDE_DIRS)
      target_include_directories(${LEROBOT_TARGET} SYSTEM
                                 PRIVATE ${LEROBOT_MEDIA_INCLUDE_DIRS})
    endif()
    target_link_libraries(${LEROBOT_TARGET} ${LEROBOT_MEDIA_LIBRARIES})
    if(LEROBOT_AV1_ENCODER STREQUAL "svt")
      target_compile_definitions(${LEROBOT_TARGET}
                                 PRIVATE LEROBOT_AV1_ENCODER_SVT=1)
    elseif(LEROBOT_AV1_ENCODER STREQUAL "libaom")
      target_compile_definitions(${LEROBOT_TARGET}
                                 PRIVATE LEROBOT_AV1_ENCODER_LIBAOM=1)
    endif()
  endforeach()
  if(VCPKG_BUILD
     AND UNIX
     AND NOT APPLE)
    # vcpkg's static FFmpeg port contains x86 assembly that requires local
    # symbol binding when it is incorporated into a shared object.
    target_link_options(lerobot_extension INTERFACE "LINKER:-Bsymbolic")
    target_link_options(lerobot_loadable_extension PRIVATE "LINKER:-Bsymbolic")
  endif()
  message(STATUS "LeRobot native FFmpeg video read/write support enabled")
elseif(LEROBOT_ENABLE_FFMPEG)
  message(
    FATAL_ERROR
      "LEROBOT_ENABLE_FFMPEG=ON requires the libavformat, libavcodec, "
      "libavutil, and libswscale development packages")
else()
  message(
    STATUS
      "LeRobot native FFmpeg video read/write support disabled by LEROBOT_ENABLE_FFMPEG=OFF"
  )
endif()

install(
  TARGETS lerobot_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
