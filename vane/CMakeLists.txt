cmake_minimum_required(VERSION 3.16)
project(LerobotVaneExtension)

option(LEROBOT_VANE_DISTRIBUTED "Build the Vane LeRobot adapter" OFF)
if(NOT LEROBOT_VANE_DISTRIBUTED OR NOT EXISTS
   "${DUCKDB_SOURCE_DIR}/src/include/duckdb/function/distributed_table_function.hpp")
  if(NOT LEROBOT_VANE_DISTRIBUTED OR NOT EXISTS
     "${DUCKDB_SOURCE_PATH}/src/include/duckdb/function/distributed_table_function.hpp")
    message(FATAL_ERROR "Use extension_config_vane.cmake with Vane's external/duckdb")
  endif()
endif()

# Reuse the original source list, dependencies and tests without editing them.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/native)
set(LEROBOT_VANE_ADAPTERS
    lerobot_extension.cpp
    function/scan/lerobot_scan.cpp
    function/video/lerobot_video_frames.cpp
    function/temporal/lerobot_temporal_targets.cpp
    function/copy/lerobot_copy.cpp)
foreach(target lerobot_extension lerobot_loadable_extension)
  get_target_property(sources ${target} SOURCES)
  foreach(source IN LISTS LEROBOT_VANE_ADAPTERS)
    list(REMOVE_ITEM sources "${source}")
    get_filename_component(adapter ${source} NAME)
    list(APPEND sources "${CMAKE_CURRENT_LIST_DIR}/../src/vane/${adapter}")
  endforeach()
  set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
  target_sources(${target} PRIVATE
                 ${CMAKE_CURRENT_LIST_DIR}/../src/vane/lerobot_snapshot.cpp)
  target_compile_definitions(${target} PRIVATE LEROBOT_VANE_DISTRIBUTED=1)
endforeach()

if(TARGET lerobot_test_extension_loader)
  set_property(TARGET lerobot_test_extension_loader PROPERTY SOURCES
               "${CMAKE_CURRENT_LIST_DIR}/../test/vane/test_extension_loader.cpp")
  target_compile_definitions(lerobot_test_extension_loader PRIVATE
                             LEROBOT_VANE_DISTRIBUTED=1)
endif()
set_target_properties(lerobot_loadable_extension PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

foreach(test video_producer nested_query)
  if(TARGET lerobot_${test}_test)
    get_target_property(sources lerobot_${test}_test SOURCES)
    list(REMOVE_ITEM sources "cpp/test_${test}.cpp")
    list(APPEND sources "${CMAKE_CURRENT_LIST_DIR}/../test/vane/test_${test}.cpp")
    set_property(TARGET lerobot_${test}_test PROPERTY SOURCES "${sources}")
    target_compile_definitions(lerobot_${test}_test PRIVATE LEROBOT_VANE_DISTRIBUTED=1)
  endif()
endforeach()
