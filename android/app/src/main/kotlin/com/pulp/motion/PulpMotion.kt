package com.pulp.motion

/**
 * Public facade over the Pulp motion observability bridge.
 *
 * Mirrors `apple/Sources/PulpSwift/PulpMotion.swift` so a developer
 * familiar with the iOS / macOS side picks this up unchanged:
 *
 * ```kotlin
 * if (PulpMotion.isTracingEnabled) {
 *     PulpMotion.publishValue(view = "Card", metric = "opacity", value = 0.5)
 * }
 *
 * PulpMotion.withProvenance(kind = "android", id = "CardView") {
 *     PulpMotion.publishValue(view = "Card", metric = "opacity", value = 1.0)
 * }
 * ```
 *
 * Off-by-default contract: every entry point first asks the installed
 * [PulpMotionBackend] whether tracing is on. When motion is disabled,
 * every call short-circuits before touching string allocations or
 * crossing JNI. A misconfigured caller that forgets the manual
 * `if (isTracingEnabled)` gate still cannot spam events in production.
 *
 * Backend wiring lives in [PulpMotionRuntime]. Production code calls
 * [installNativeBackend] once (typically from `PulpApplication` after
 * `System.loadLibrary("pulp")`). JVM unit tests install a recording
 * backend via `PulpMotionRuntime.installBackend(...)` and never load
 * the native library at all.
 */
object PulpMotion {

    /**
     * `true` when the process-wide motion Coordinator is recording.
     * Hot loops that build expensive metric inputs (string
     * concatenation, vector serialization) should gate on this — the
     * publish methods themselves also short-circuit but only after
     * their arguments have been evaluated.
     */
    val isTracingEnabled: Boolean
        get() = PulpMotionRuntime.backend.isTracingEnabled()

    /**
     * Publish a single scalar value. No-op when tracing is off.
     */
    fun publishValue(
        view: String,
        metric: String,
        value: Double,
        epsilon: Double = DEFAULT_EPSILON,
        precision: Int = DEFAULT_PRECISION,
    ) {
        val b = PulpMotionRuntime.backend
        if (!b.isTracingEnabled()) return
        b.publishValue(view, metric, value, epsilon, precision)
    }

    /**
     * Publish a multi-component value (e.g. a 2D point or a geometry
     * rect). Components are sorted by name inside the coordinator so
     * downstream log lines stay stable.
     */
    fun publishComponents(
        view: String,
        metric: String,
        components: List<Pair<String, Double>>,
        epsilon: Double = DEFAULT_EPSILON,
        precision: Int = DEFAULT_PRECISION,
    ) {
        val b = PulpMotionRuntime.backend
        if (!b.isTracingEnabled()) return
        b.publishComponents(view, metric, components, epsilon, precision)
    }

    /**
     * Stamp every subsequent publish on this thread of work with the
     * supplied provenance envelope. Pair with [clearAmbientProvenance]
     * to scope the stamp — prefer the [withProvenance] block when the
     * scope fits on the call stack.
     */
    fun setAmbientProvenance(
        kind: String,
        id: String,
        file: String = "",
        line: Int = 0,
    ) {
        PulpMotionRuntime.backend.setAmbientProvenance(kind, id, file, line)
    }

    /** Clear the ambient provenance slot. Idempotent. */
    fun clearAmbientProvenance() {
        PulpMotionRuntime.backend.clearAmbientProvenance()
    }

    /**
     * Scoped helper — set ambient provenance, run [block], clear it
     * even if [block] throws. Single-threaded by design (the
     * process-wide ambient slot is not coroutine-safe; do not call
     * from suspending code that may switch dispatchers inside the
     * block). The Swift bridge follows the same constraint.
     */
    inline fun <R> withProvenance(
        kind: String,
        id: String,
        file: String = "",
        line: Int = 0,
        block: () -> R,
    ): R {
        setAmbientProvenance(kind, id, file, line)
        return try {
            block()
        } finally {
            clearAmbientProvenance()
        }
    }

    /**
     * Register a geometry trace. Returns a positive trace id, or `0`
     * when tracing is off / the underlying coordinator declined to
     * register. Most callers prefer the `View.pulpMotionTrace(...)`
     * extension or the Compose `Modifier.pulpMotionGeometry(...)` —
     * this entry point is here for custom probes that need to bypass
     * both (e.g. a `Canvas` overlay, a SurfaceView driver).
     */
    fun registerGeometryTrace(view: String, fps: Int = DEFAULT_FPS): Int =
        PulpMotionRuntime.backend.registerGeometryTrace(view, fps)

    /**
     * Push a new geometry frame for a previously registered trace.
     * `metricName` defaults to `"frame"`; pass a different name to
     * route the rect through the publish channel as a separate
     * metric (the registered "frame" sampler still fires on the next
     * FrameClock tick).
     */
    fun updateGeometry(
        traceId: Int,
        metricName: String = "frame",
        minX: Double,
        minY: Double,
        width: Double,
        height: Double,
    ) {
        if (traceId == 0) return
        PulpMotionRuntime.backend.updateGeometry(
            traceId, metricName, minX, minY, width, height,
        )
    }

    /** Detach a previously registered trace. Idempotent. */
    fun detachTrace(traceId: Int) {
        if (traceId == 0) return
        PulpMotionRuntime.backend.detachTrace(traceId)
    }

    // ── Backend wiring ──────────────────────────────────────────────

    /**
     * Install the production [PulpMotionBackend] that forwards every
     * call into the JNI shims on [PulpMotionNative]. Call this from
     * `PulpApplication.onCreate` AFTER `System.loadLibrary("pulp")`.
     *
     * Test code should NOT call this — install a recording backend
     * directly via [PulpMotionRuntime.installBackend] instead.
     */
    fun installNativeBackend() {
        PulpMotionRuntime.installBackend(
            PulpMotionBackend(
                isTracingEnabled = { PulpMotionNative.nativeTracingEnabled() },
                publishValue = { v, m, value, eps, pr ->
                    PulpMotionNative.nativePublishValue(v, m, value, eps, pr)
                },
                publishComponents = { v, m, comps, eps, pr ->
                    val keys = Array(comps.size) { comps[it].first }
                    val values = DoubleArray(comps.size) { comps[it].second }
                    PulpMotionNative.nativePublishComponents(
                        v, m, keys, values, eps, pr,
                    )
                },
                setAmbientProvenance = { kind, id, file, line ->
                    PulpMotionNative.nativeSetAmbientProvenance(kind, id, file, line)
                },
                clearAmbientProvenance = {
                    PulpMotionNative.nativeClearAmbientProvenance()
                },
                registerGeometryTrace = { v, fps ->
                    PulpMotionNative.nativeRegisterGeometryTrace(v, fps)
                },
                updateGeometry = { id, m, x, y, w, h ->
                    PulpMotionNative.nativeUpdateGeometry(id, m, x, y, w, h)
                },
                detachTrace = { id ->
                    PulpMotionNative.nativeDetachTrace(id)
                },
            ),
        )
    }

    const val DEFAULT_EPSILON: Double = 0.0001
    const val DEFAULT_PRECISION: Int = 3
    const val DEFAULT_FPS: Int = 30
}
