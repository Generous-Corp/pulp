package com.pulp.bridge

import android.content.Context

/**
 * JNI bridge between Kotlin and the Pulp C++ core.
 * All native methods are implemented in core/platform/src/android/jni_bridge.cpp.
 */
object PulpBridge {

    /**
     * Initialize the Pulp native engine.
     * Must be called after System.loadLibrary("pulp") and before any other native calls.
     */
    external fun nativeInit(context: Context)
}
