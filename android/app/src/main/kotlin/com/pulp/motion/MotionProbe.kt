package com.pulp.motion

import android.graphics.Rect
import android.view.View
import android.view.ViewTreeObserver

/**
 * RAII handle for a single attached motion trace.
 *
 * Returned by [pulpMotionTrace]. When [close] is called (directly or
 * via `Closeable.use { }`), the underlying trace is detached and the
 * `OnPreDrawListener` / `OnAttachStateChangeListener` are removed.
 *
 * Mirrors the Swift bridge's `PulpMotionGeometryProbe` — the difference
 * is the lifecycle hook: Android Views don't have `deinit`, so Kotlin
 * spells it `AutoCloseable`. Auto-detach also fires when the host
 * View is removed from the window (`onViewDetachedFromWindow`).
 */
class PulpMotionGeometryProbe internal constructor(
    private val target: View,
    private val traceId: Int,
    private val metrics: List<MotionMetric>,
    private val preDrawListener: ViewTreeObserver.OnPreDrawListener,
    private val attachListener: View.OnAttachStateChangeListener,
) : AutoCloseable {

    private var closed = false

    /** `true` while the underlying motion trace is still attached. */
    val isAttached: Boolean
        get() = !closed && traceId != 0

    /**
     * Push a new rect manually — for callers that animate geometry
     * outside the normal layout pipeline (a `SurfaceView` overlay, a
     * physics simulation, etc.). Most callers should let the
     * `OnPreDrawListener` installed by [pulpMotionTrace] feed the
     * trace automatically.
     */
    fun update(metricName: String = "frame", rect: Rect) {
        if (!isAttached) return
        PulpMotion.updateGeometry(
            traceId = traceId,
            metricName = metricName,
            minX = rect.left.toDouble(),
            minY = rect.top.toDouble(),
            width = (rect.right - rect.left).toDouble(),
            height = (rect.bottom - rect.top).toDouble(),
        )
    }

    override fun close() {
        if (closed) return
        closed = true
        // Best-effort listener teardown; the ViewTreeObserver may have
        // already been detached by the framework on view removal, in
        // which case `isAlive` returns false.
        val vto = target.viewTreeObserver
        if (vto.isAlive) vto.removeOnPreDrawListener(preDrawListener)
        target.removeOnAttachStateChangeListener(attachListener)
        PulpMotion.detachTrace(traceId)
    }
}

/**
 * Attach a Pulp motion trace to this [View].
 *
 * Returns `null` when [PulpMotion.isTracingEnabled] is false at attach
 * time — production builds with motion disabled pay one branch per
 * call to this extension and nothing else.
 *
 * Behavior when tracing is enabled:
 *
 *  - Registers a geometry trace stamped with `source_kind="android"`
 *    provenance (set on the C bridge).
 *  - Installs a `ViewTreeObserver.OnPreDrawListener` that computes the
 *    view's window-space rect via `getLocationInWindow()` and forwards
 *    every change to `PulpMotion.updateGeometry`. PreDraw catches
 *    intra-frame translation / scroll that `OnGlobalLayoutListener`
 *    would miss (per Codex's locked-in design feedback).
 *  - Publishes each [MotionMetric.Value] declared in [build] once at
 *    attach time. Live scalars should re-publish from your own
 *    state — the trace itself just owns geometry sampling.
 *  - Auto-detaches when the View is removed from the window via
 *    `OnAttachStateChangeListener.onViewDetachedFromWindow`. The
 *    returned handle's `close()` also detaches eagerly.
 *
 * @param name Logical identifier emitted as the trace's view name.
 * @param fps Sampler tick rate. Mirrors `motion::TraceOptions::fps`.
 * @param build Trace-block builder — see [motionTrace].
 */
fun View.pulpMotionTrace(
    name: String,
    fps: Int = PulpMotion.DEFAULT_FPS,
    build: MotionTraceScope.() -> Unit,
): PulpMotionGeometryProbe? {
    if (!PulpMotion.isTracingEnabled) return null

    val metrics = motionTrace(build)
    val traceId = PulpMotion.registerGeometryTrace(name, fps)
    if (traceId == 0) return null

    // Publish initial scalar values once at attach time, stamped with
    // android-style provenance so the TraceStarted event in the
    // fixture carries the host view name.
    PulpMotion.withProvenance(kind = "android", id = name) {
        for (m in metrics) {
            if (m is MotionMetric.Value) {
                PulpMotion.publishValue(
                    view = name,
                    metric = m.name,
                    value = m.value,
                    epsilon = m.epsilon,
                    precision = m.precision,
                )
            }
        }
    }

    val locationBuffer = IntArray(2)
    val target = this
    val geometryMetricNames = metrics.mapNotNull { metric ->
        when (metric) {
            is MotionMetric.Geometry -> metric.name
            is MotionMetric.ScrollGeometry -> metric.name
            else -> null
        }
    }.ifEmpty { listOf("frame") }

    val preDrawListener = ViewTreeObserver.OnPreDrawListener {
        target.getLocationInWindow(locationBuffer)
        val x = locationBuffer[0].toDouble()
        val y = locationBuffer[1].toDouble()
        val w = target.width.toDouble()
        val h = target.height.toDouble()
        for (mName in geometryMetricNames) {
            PulpMotion.updateGeometry(
                traceId = traceId,
                metricName = mName,
                minX = x, minY = y,
                width = w, height = h,
            )
        }
        // Returning true keeps the draw pass scheduled — we only sample.
        true
    }

    val attachListener = object : View.OnAttachStateChangeListener {
        override fun onViewAttachedToWindow(v: View) = Unit
        override fun onViewDetachedFromWindow(v: View) {
            // Auto-detach: mirrors Swift's onDisappear in
            // PulpMotionTraceModifier.
            v.viewTreeObserver.takeIf { it.isAlive }
                ?.removeOnPreDrawListener(preDrawListener)
            v.removeOnAttachStateChangeListener(this)
            PulpMotion.detachTrace(traceId)
        }
    }

    viewTreeObserver.addOnPreDrawListener(preDrawListener)
    addOnAttachStateChangeListener(attachListener)

    return PulpMotionGeometryProbe(
        target = this,
        traceId = traceId,
        metrics = metrics,
        preDrawListener = preDrawListener,
        attachListener = attachListener,
    )
}
