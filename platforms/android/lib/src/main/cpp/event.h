// Derived from libmpv-android's JNI glue (MIT; Copyright (c) 2016 Ilya
// Zhuravlev, Copyright (c) 2016 sfan5); see platforms/android/lib/NOTICE.

#pragma once

/**
 * The session's event loop. [arg] is the `Session*` that nativeInit started it
 * for; the thread borrows that session's immutable handle and id until the
 * retirement that frees it joins this thread.
 */
void* event_thread(void* arg);
