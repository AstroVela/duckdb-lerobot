# Keep the probe linked to exactly the same FFmpeg libraries as both extension
# targets.
set(LEROBOT_GPL_FEATURE OFF)
if("gpl-codecs" IN_LIST VCPKG_MANIFEST_FEATURES)
  set(LEROBOT_GPL_FEATURE ON)
endif()
option(LEROBOT_ALLOW_GPL
       "Allow GPL FFmpeg for explicit development/custom builds" OFF)
option(LEROBOT_PACKAGE_RELEASE
       "Build the default LGPL distribution and corresponding source bundle"
       OFF)

if(LEROBOT_PACKAGE_RELEASE AND (LEROBOT_ALLOW_GPL OR LEROBOT_GPL_FEATURE))
  message(FATAL_ERROR "The default distribution cannot include GPL codecs")
endif()
if(LEROBOT_PACKAGE_RELEASE AND NOT VCPKG_BUILD)
  message(
    FATAL_ERROR "LEROBOT_PACKAGE_RELEASE requires the pinned vcpkg toolchain")
endif()

add_executable(lerobot_ffmpeg_license_probe EXCLUDE_FROM_ALL
               ${CMAKE_CURRENT_LIST_DIR}/../src/ffmpeg_license_probe.cpp)
target_include_directories(lerobot_ffmpeg_license_probe SYSTEM
                           PRIVATE ${LEROBOT_MEDIA_INCLUDE_DIRS})
target_link_libraries(lerobot_ffmpeg_license_probe ${LEROBOT_MEDIA_LIBRARIES})
set(LEROBOT_LICENSE_PROBE_ARGS "")
if(LEROBOT_ALLOW_GPL OR LEROBOT_GPL_FEATURE)
  list(APPEND LEROBOT_LICENSE_PROBE_ARGS --allow-gpl)
endif()
add_custom_target(
  lerobot_check_ffmpeg_license ALL
  COMMAND
    $<TARGET_FILE:lerobot_ffmpeg_license_probe> ${LEROBOT_LICENSE_PROBE_ARGS}
    --output ${CMAKE_BINARY_DIR}/lerobot-ffmpeg-license.json
  DEPENDS lerobot_ffmpeg_license_probe
  COMMENT "Check the license of the linked FFmpeg libraries"
  VERBATIM)
add_dependencies(lerobot_extension lerobot_check_ffmpeg_license)
add_dependencies(lerobot_loadable_extension lerobot_check_ffmpeg_license)

if(LEROBOT_PACKAGE_RELEASE)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  get_filename_component(LEROBOT_SOURCE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..
                         ABSOLUTE)
  file(
    GENERATE
    OUTPUT ${CMAKE_BINARY_DIR}/lerobot-ffmpeg-link-libraries.txt
    CONTENT
      "$<JOIN:$<TARGET_GENEX_EVAL:lerobot_ffmpeg_license_probe,$<TARGET_PROPERTY:lerobot_ffmpeg_license_probe,LINK_LIBRARIES>>,\n>\n"
  )
  add_custom_target(
    lerobot_release_bundle ALL
    COMMAND
      ${Python3_EXECUTABLE} ${LEROBOT_SOURCE_ROOT}/scripts/package_release.py
      --source-root ${LEROBOT_SOURCE_ROOT} --build-dir ${CMAKE_BINARY_DIR}
      --vcpkg-root ${Z_VCPKG_ROOT_DIR} --installed-dir ${VCPKG_INSTALLED_DIR}
      --triplet ${VCPKG_TARGET_TRIPLET} --extension
      $<TARGET_FILE:lerobot_loadable_extension>
    DEPENDS lerobot_loadable_extension lerobot_check_ffmpeg_license
    COMMENT "Package the extension's corresponding source and license materials"
    VERBATIM)
endif()
