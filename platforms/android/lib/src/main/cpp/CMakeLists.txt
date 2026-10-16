# Derived from libmpv-android's build of its JNI glue (MIT; Copyright (c) 2016
# Ilya Zhuravlev, Copyright (c) 2016 sfan5); see platforms/android/lib/NOTICE.
#
# libplayer.so, the JNI glue MpvPlayer loads with System.loadLibrary("player")
# after libmpv.so. It links the libmpv.so and libavcodec.so of the mpv-build
# per-ABI tarballs, and compiles against the headers they ship:
#
#   MPV_NATIVE_ROOT/<ANDROID_ABI>/lib/      libmpv.so, libavcodec.so, ...
#   MPV_NATIVE_ROOT/<ANDROID_ABI>/include/  mpv/*.h, libavcodec/jni.h, ...
cmake_minimum_required(VERSION 3.22.1)

project(mpvjni LANGUAGES CXX)

if(NOT MPV_NATIVE_ROOT)
  message(FATAL_ERROR
    "MPV_NATIVE_ROOT is not set. Pass -DMPV_NATIVE_ROOT=<dir>, where <dir>/<ANDROID_ABI>/lib holds the "
    "mpv-build tarball's libmpv.so and libavcodec.so and <dir>/<ANDROID_ABI>/include its mpv and FFmpeg headers.")
endif()

set(MPV_NATIVE_LIB_DIR "${MPV_NATIVE_ROOT}/${ANDROID_ABI}/lib")
set(MPV_NATIVE_INCLUDE_DIR "${MPV_NATIVE_ROOT}/${ANDROID_ABI}/include")
foreach(required
    "${MPV_NATIVE_LIB_DIR}/libmpv.so"
    "${MPV_NATIVE_LIB_DIR}/libavcodec.so"
    "${MPV_NATIVE_INCLUDE_DIR}/mpv/client.h"
    "${MPV_NATIVE_INCLUDE_DIR}/libavcodec/jni.h")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR
      "${required} does not exist. MPV_NATIVE_ROOT (${MPV_NATIVE_ROOT}) must hold the extracted mpv-build "
      "android tarball for ABI '${ANDROID_ABI}' under ${ANDROID_ABI}/lib and ${ANDROID_ABI}/include.")
  endif()
endforeach()

add_library(player SHARED
  main.cpp
  render.cpp
  log.cpp
  jni_utils.cpp
  property.cpp
  event.cpp
  session.cpp)

add_library(mpv SHARED IMPORTED)
set_target_properties(mpv PROPERTIES IMPORTED_LOCATION "${MPV_NATIVE_LIB_DIR}/libmpv.so")

# av_jni_set_java_vm, av_jni_set_android_app_ctx and
# av_jni_set_mediacodec_callback_class (main.cpp) live in FFmpeg's libavcodec,
# which the same tarballs package.
add_library(avcodec SHARED IMPORTED)
set_target_properties(avcodec PROPERTIES IMPORTED_LOCATION "${MPV_NATIVE_LIB_DIR}/libavcodec.so")

set_target_properties(player PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_include_directories(player PRIVATE "${MPV_NATIVE_INCLUDE_DIR}")
# The prefix map keeps the checkout path out of the binary, so the same sources
# build to the same bytes wherever they are checked out.
target_compile_options(player PRIVATE -Wall -Werror "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.")
target_link_libraries(player PRIVATE mpv avcodec log)
