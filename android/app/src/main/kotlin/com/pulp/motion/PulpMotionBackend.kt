package com.pulp.motion

/**
 * Closure-bag the host application installs to bridge Kotlin into the
 * `pulp::view::motion` C ABI. Mirrors Swift's `PulpMotionBackend` so
 * the Android and Apple bridges have the same testability story:
 *
 *  - In production, [PulpMotion.installNativeBackend] wires every
 *    closure to the matching [PulpMotionNative] JNI call. The C bridge
 *    in turn double-checks `motion::Coordinator::tracing_enabled()`,
 *    so a misconfigured backend cannot spam events when motion is off.
 *
 *  - In JVM unit tests, install a recording backend that captures
 *    every call into a buffer. `gradle test` exercises the facade
 *    end-to-end without ever calling `System.loadLibrary("pulp")`.
 *
 * Every field defaults to a no-op so a partially-installed backend
 * still degrades safely — `PulpMotion.publishValue` short-circuits
 * before touching most fields when [isTracingEnabled] returns false.
 */
data class PulpMotionBackend(
    val isTracingEnabled: () -> Boolean = { false },
    val publishValue: (
        view: String,
        metric: String,
        value: Double,
        epsilon: Double,
        precision: Int,
    ) -> Unit = { _, _, _, _, _ -> },
    val publishComponents: (
        view: String,
        metric: String,
        components: List<Pair<String, Double>>,
        epsilon: Double,
        precision: Int,
    ) -> Unit = { _, _, _, _, _ -> },
    val setAmbientProvenance: (
        kind: String,
        id: String,
        file: String,
        line: Int,
    ) -> Unit = { _, _, _, _ -> },
    val clearAmbientProvenance: () -> Unit = {},
    val registerGeometryTrace: (
        view: String,
        fps: Int,
    ) -> Int = { _, _ -> 0 },
    val updateGeometry: (
        traceId: Int,
        metric: String,
        minX: Double,
        minY: Double,
        width: Double,
        height: Double,
    ) -> Unit = { _, _, _, _, _, _ -> },
    val detachTrace: (traceId: Int) -> Unit = { _ -> },
) {
    companion object {
        /** No-op backend — the default when no host has wired one in. */
        fun noOp(): PulpMotionBackend = PulpMotionBackend()
    }
}

/**
 * Process-wide accessor for the active [PulpMotionBackend]. The host
 * app calls [installBackend] at launch (see
 * [PulpMotion.installNativeBackend] for the JNI wiring helper). Tests
 * install a recording backend instead.
 *
 * Thread-safety: the accessor is synchronized on this object's
 * monitor. The backend itself can be invoked from any thread, but the
 * closure bag is reassigned atomically.
 */
object PulpMotionRuntime {
    @Volatile
    private var current: PulpMotionBackend = PulpMotionBackend.noOp()

    /**
     * Install [backend], or pass `null` to revert to the no-op default
     * (useful between tests to drop any captured state).
     */
    @Synchronized
    fun installBackend(backend: PulpMotionBackend?) {
        current = backend ?: PulpMotionBackend.noOp()
    }

    /** Read the currently installed backend. */
    val backend: PulpMotionBackend
        get() = current
}
