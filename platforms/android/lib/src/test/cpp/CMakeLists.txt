# Host tests for the JNI glue in ../../main/cpp. They compile the production
# sources against fakes of jni.h and android/log.h, with the real mpv and
# FFmpeg headers of the pinned trees (scripts/test_android_glue.sh fetches
# them), so no Android toolchain or device is needed.
cmake_minimum_required(VERSION 3.22.1)
project(mpv_android_glue_tests LANGUAGES CXX)

set(MPV_INCLUDE_DIR "" CACHE PATH "Directory containing mpv/client.h (the include/ dir of the pinned mpv tree)")
set(FFMPEG_INCLUDE_DIR "" CACHE PATH "Directory containing libavcodec/jni.h (the root of the patched FFmpeg tree)")
if(NOT EXISTS "${MPV_INCLUDE_DIR}/mpv/client.h")
  message(FATAL_ERROR "MPV_INCLUDE_DIR must name a directory containing mpv/client.h (got '${MPV_INCLUDE_DIR}')")
endif()
if(NOT EXISTS "${FFMPEG_INCLUDE_DIR}/libavcodec/jni.h")
  message(FATAL_ERROR "FFMPEG_INCLUDE_DIR must name a directory containing libavcodec/jni.h (got '${FFMPEG_INCLUDE_DIR}')")
endif()

set(GLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../main/cpp")

# Only the one installed FFmpeg header the glue includes is staged: a source
# tree root on the include path would let its top-level files (a release
# tarball's VERSION, say, on a case-insensitive file system) shadow standard
# headers.
set(FFMPEG_STAGED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/ffmpeg-include")
configure_file("${FFMPEG_INCLUDE_DIR}/libavcodec/jni.h" "${FFMPEG_STAGED_INCLUDE}/libavcodec/jni.h" COPYONLY)

enable_testing()
find_package(Threads REQUIRED)

add_executable(mpv_utf8_convert_test mpv_utf8_convert_test.cpp)
target_compile_features(mpv_utf8_convert_test PRIVATE cxx_std_17)
add_test(NAME mpv_utf8_convert_test COMMAND mpv_utf8_convert_test)

add_executable(mpv_lifecycle_test
  mpv_lifecycle_test.cpp
  # The JNI entries the test compiles call into the session registry, which is
  # a separate translation unit in the production library.
  "${GLUE_DIR}/session.cpp"
  fakes/host_shims.cpp)
target_compile_features(mpv_lifecycle_test PRIVATE cxx_std_17)
# The fakes come first so <jni.h> and <android/log.h> resolve to them.
target_include_directories(mpv_lifecycle_test PRIVATE fakes "${MPV_INCLUDE_DIR}" "${FFMPEG_STAGED_INCLUDE}")
target_link_libraries(mpv_lifecycle_test PRIVATE Threads::Threads)
add_test(NAME mpv_lifecycle_test COMMAND mpv_lifecycle_test)
set_tests_properties(mpv_lifecycle_test PROPERTIES TIMEOUT 30)

add_executable(mpv_event_order_test
  mpv_event_order_test.cpp
  "${GLUE_DIR}/session.cpp")
target_compile_features(mpv_event_order_test PRIVATE cxx_std_17)
target_include_directories(mpv_event_order_test PRIVATE fakes "${MPV_INCLUDE_DIR}")
target_link_libraries(mpv_event_order_test PRIVATE Threads::Threads)
add_test(NAME mpv_event_order_test COMMAND mpv_event_order_test)
set_tests_properties(mpv_event_order_test PROPERTIES TIMEOUT 30)
