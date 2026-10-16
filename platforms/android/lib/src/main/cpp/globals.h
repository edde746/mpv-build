// Derived from libmpv-android's JNI glue (MIT; Copyright (c) 2016 Ilya
// Zhuravlev, Copyright (c) 2016 sfan5); see platforms/android/lib/NOTICE.

#pragma once

#include <jni.h>

// The process's JavaVM, published once by the first nativeCreate (see
// prepare_environment) and read by every thread that needs a JNIEnv.
extern JavaVM* g_vm;
