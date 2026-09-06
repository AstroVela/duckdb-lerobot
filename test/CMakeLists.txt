# These tests compile selected LeRobot implementations (and a controlled codec)
# into their executables. Link DuckDB's static core so Windows does not require
# internal symbols that are absent from duckdb.dll's export table. Use a loader
# containing only the native readers to avoid linking a second LeRobot copy.
add_library(lerobot_test_duckdb INTERFACE)
if(BUILD_MAIN_DUCKDB_LIBRARY)
  add_library(lerobot_test_extension_loader OBJECT
              cpp/test_extension_loader.cpp)
  target_include_directories(
    lerobot_test_extension_loader
    PRIVATE "${DUCKDB_MODULE_BASE_DIR}/extension/core_functions/include"
            "${DUCKDB_MODULE_BASE_DIR}/extension/json/include"
            "${DUCKDB_MODULE_BASE_DIR}/extension/parquet/include")
  target_compile_definitions(lerobot_test_extension_loader
                             PRIVATE DUCKDB_STATIC_BUILD)
  target_compile_definitions(lerobot_test_duckdb INTERFACE DUCKDB_STATIC_BUILD)
  target_sources(lerobot_test_duckdb
                 INTERFACE $<TARGET_OBJECTS:lerobot_test_extension_loader>)
  target_link_libraries(
    lerobot_test_duckdb INTERFACE duckdb_static core_functions_extension
                                  json_extension parquet_extension)
else()
  # Support the small local harness that imports an existing DuckDB library.
  target_link_libraries(lerobot_test_duckdb INTERFACE duckdb)
endif()

# Compile the real executor with a controlled encoder, so cancellation tests can
# hold workers at exact boundaries without depending on FFmpeg timing.
add_executable(
  lerobot_codec_executor_test cpp/test_codec_executor.cpp
                              ../src/function/copy/lerobot_codec_executor.cpp)
target_include_directories(
  lerobot_codec_executor_test
  PRIVATE ../src/include "${DUCKDB_MODULE_BASE_DIR}/third_party/catch")
target_link_libraries(lerobot_codec_executor_test PRIVATE lerobot_test_duckdb)
# Keep the test encoder separate from any LeRobot symbols linked into DuckDB.
set_target_properties(
  lerobot_codec_executor_test PROPERTIES CXX_VISIBILITY_PRESET hidden
                                         VISIBILITY_INLINES_HIDDEN ON)

add_custom_target(
  test_lerobot_codec_executor
  COMMAND $<TARGET_FILE:lerobot_codec_executor_test>
  DEPENDS lerobot_codec_executor_test
  USES_TERMINAL)

# Include the producer implementation in the test translation unit to exercise
# its private lifecycle with DuckDB's real Executor::CancelTasks. Keep the
# source-only path independent of FFmpeg and of public SQL testing hooks.
add_executable(
  lerobot_video_producer_test
  cpp/test_video_producer.cpp
  ../src/lerobot_path.cpp
  ../src/lerobot_query.cpp
  ../src/function/lerobot_schema.cpp
  ../src/function/temporal/lerobot_temporal_targets.cpp
  ../src/function/video/lerobot_video_options.cpp
  ../src/storage/lerobot_metadata_cache.cpp
  ../src/storage/lerobot_timestamp_lookup.cpp)
target_include_directories(
  lerobot_video_producer_test
  PRIVATE ../src/include "${DUCKDB_MODULE_BASE_DIR}/third_party/catch")
target_link_libraries(lerobot_video_producer_test PRIVATE lerobot_test_duckdb)
set_target_properties(
  lerobot_video_producer_test PROPERTIES CXX_VISIBILITY_PRESET hidden
                                         VISIBILITY_INLINES_HIDDEN ON)

add_custom_target(
  test_lerobot_video_producer
  COMMAND $<TARGET_FILE:lerobot_video_producer_test>
  DEPENDS lerobot_video_producer_test
  USES_TERMINAL)

add_executable(
  lerobot_nested_query_test
  cpp/test_nested_query.cpp
  ../src/lerobot_path.cpp
  ../src/lerobot_query.cpp
  ../src/function/lerobot_schema.cpp
  ../src/function/temporal/lerobot_temporal_targets.cpp
  ../src/function/video/lerobot_video_options.cpp
  ../src/storage/lerobot_metadata_cache.cpp
  ../src/storage/lerobot_timestamp_lookup.cpp)
target_include_directories(
  lerobot_nested_query_test
  PRIVATE ../src/include "${DUCKDB_MODULE_BASE_DIR}/third_party/catch")
target_link_libraries(lerobot_nested_query_test PRIVATE lerobot_test_duckdb)
set_target_properties(
  lerobot_nested_query_test PROPERTIES CXX_VISIBILITY_PRESET hidden
                                       VISIBILITY_INLINES_HIDDEN ON)

add_custom_target(
  test_lerobot_nested_query
  COMMAND $<TARGET_FILE:lerobot_nested_query_test>
  DEPENDS lerobot_nested_query_test
  USES_TERMINAL)

# Register production COPY in an isolated database and intercept filesystem
# operations. Numeric statistics, binding and cleanup run without video support.
add_executable(
  lerobot_video_io_test
  cpp/test_video_io.cpp
  cpp/test_copy_bind.cpp
  cpp/test_numeric_stats.cpp
  cpp/test_image_writer.cpp
  ../src/lerobot_path.cpp
  ../src/lerobot_query.cpp
  ../src/function/lerobot_schema.cpp
  ../src/function/copy/lerobot_copy.cpp
  ../src/function/copy/lerobot_copy_options.cpp
  ../src/function/copy/lerobot_numeric_stats.cpp
  ../src/function/copy/lerobot_codec_executor.cpp
  ../src/function/copy/lerobot_video_io.cpp
  ../src/function/copy/lerobot_video_writer.cpp)
target_include_directories(
  lerobot_video_io_test PRIVATE ../src/include
                                "${DUCKDB_MODULE_BASE_DIR}/third_party/catch")
target_link_libraries(lerobot_video_io_test PRIVATE lerobot_test_duckdb)
if(LEROBOT_MEDIA_LIBRARIES)
  if(LEROBOT_MEDIA_INCLUDE_DIRS)
    target_include_directories(lerobot_video_io_test SYSTEM
                               PRIVATE ${LEROBOT_MEDIA_INCLUDE_DIRS})
  endif()
  target_compile_definitions(lerobot_video_io_test
                             PRIVATE LEROBOT_HAVE_FFMPEG=1)
  target_link_libraries(lerobot_video_io_test
                        PRIVATE ${LEROBOT_MEDIA_LIBRARIES})
endif()
set_target_properties(
  lerobot_video_io_test PROPERTIES CXX_VISIBILITY_PRESET hidden
                                   VISIBILITY_INLINES_HIDDEN ON)
add_custom_target(
  test_lerobot_video_io
  COMMAND $<TARGET_FILE:lerobot_video_io_test>
  DEPENDS lerobot_video_io_test
  USES_TERMINAL)
add_custom_target(
  test_lerobot_copy_cleanup
  COMMAND $<TARGET_FILE:lerobot_video_io_test> "[copy_cleanup]"
  DEPENDS lerobot_video_io_test
  USES_TERMINAL)
add_custom_target(
  test_lerobot_copy_bind
  COMMAND $<TARGET_FILE:lerobot_video_io_test> "[copy_bind]"
  DEPENDS lerobot_video_io_test
  USES_TERMINAL)
add_custom_target(
  test_lerobot_numeric_stats
  COMMAND $<TARGET_FILE:lerobot_video_io_test> "[numeric_stats]"
  DEPENDS lerobot_video_io_test
  USES_TERMINAL)
add_custom_target(
  test_lerobot_image_writer
  COMMAND $<TARGET_FILE:lerobot_video_io_test> "[image_writer],[image_copy]"
  DEPENDS lerobot_video_io_test
  USES_TERMINAL)
