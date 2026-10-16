// Derived from libmpv-android's JNI glue (MIT; Copyright (c) 2016 Ilya
// Zhuravlev, Copyright (c) 2016 sfan5); see platforms/android/lib/NOTICE.

#pragma once

#include <android/log.h>

#define LOG_TAG "mpv"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

void die(const char* msg);
