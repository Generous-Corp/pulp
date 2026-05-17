package com.pulp.motion

/**
 * Internal JNI surface for `pulp::view::motion` on Android.
 *
 * Every method here maps 1:1 to a `pulp_motion_*` C ABI entry point in
 * `core/platform/src/android/jni_motion.cpp` (declared in
 * `core/platform/include/pulp/platform/android/motion_bridge.h`). The
 * JNI shim names follow the standard mangling
 * (`Java_com_pulp_motion_PulpMotionNative_nativeFoo`).
 *
 * Callers should not use this directly — go through [PulpMotion]
 * instead, which gates every call on the backend seam so JVM unit
 * tests run without loading libpulp.so.
 */
internal object PulpMotionNative {
    external fun nativeTracingEnabled(): Boolean

    external fun nativePublishValue(
        viewName: String,
        metricName: String,
        value: Double,
        epsilon: Double,
        precision: Int,
    )

    external fun nativePublishComponents(
        viewName: String,
        metricName: String,
        keys: Array<String>,
        values: DoubleArray,
        epsilon: Double,
        precision: Int,
    )

    external fun nativeSetAmbientProvenance(
        kind: String,
        id: String,
        file: String,
        line: Int,
    )

    external fun nativeClearAmbientProvenance()

    external fun nativeRegisterGeometryTrace(viewName: String, fps: Int): Int

    external fun nativeUpdateGeometry(
        traceId: Int,
        metricName: String,
        minX: Double,
        minY: Double,
        width: Double,
        height: Double,
    )

    external fun nativeDetachTrace(traceId: Int)
}
