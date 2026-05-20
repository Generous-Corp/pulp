package com.pulp.motion

import android.graphics.Rect
import android.graphics.RectF

/**
 * Geometry property selector for [Trace.geometry]. Mirrors the names
 * used by the C++ side (`pulp::view::motion::GeometryProperty`) and by
 * the Swift facade (`MotionGeometryProperty`).
 */
@JvmInline
value class MotionGeometryProperty(val rawValue: String) {
    companion object {
        val minX   = MotionGeometryProperty("minX")
        val minY   = MotionGeometryProperty("minY")
        val maxX   = MotionGeometryProperty("maxX")
        val maxY   = MotionGeometryProperty("maxY")
        val midX   = MotionGeometryProperty("midX")
        val midY   = MotionGeometryProperty("midY")
        val width  = MotionGeometryProperty("width")
        val height = MotionGeometryProperty("height")
    }
}

/** Marker for the [motionTrace] DSL. Keeps inner builders from
 *  accidentally extending the outer one. */
@DslMarker
annotation class MotionTraceBuilder

/**
 * A single metric inside a [motionTrace] block. Created via the
 * static factories on [Trace] (`.value(...)`, `.geometry(...)`,
 * `.scrollGeometry(...)`).
 */
sealed class MotionMetric {
    abstract val name: String
    abstract val epsilon: Double
    abstract val precision: Int

    data class Value(
        override val name: String,
        val value: Double,
        override val epsilon: Double = PulpMotion.DEFAULT_EPSILON,
        override val precision: Int = PulpMotion.DEFAULT_PRECISION,
    ) : MotionMetric()

    data class Geometry(
        override val name: String,
        val properties: List<MotionGeometryProperty> = DEFAULT_PROPERTIES,
        override val epsilon: Double = DEFAULT_GEOMETRY_EPSILON,
        override val precision: Int = DEFAULT_GEOMETRY_PRECISION,
    ) : MotionMetric() {
        companion object {
            val DEFAULT_PROPERTIES: List<MotionGeometryProperty> = listOf(
                MotionGeometryProperty.minX,
                MotionGeometryProperty.minY,
                MotionGeometryProperty.width,
                MotionGeometryProperty.height,
            )
        }
    }

    data class ScrollGeometry(
        override val name: String,
        override val epsilon: Double = DEFAULT_GEOMETRY_EPSILON,
        override val precision: Int = DEFAULT_GEOMETRY_PRECISION,
    ) : MotionMetric()

    companion object {
        const val DEFAULT_GEOMETRY_EPSILON: Double = 0.1
        const val DEFAULT_GEOMETRY_PRECISION: Int = 2
    }
}

/**
 * Public factory for [MotionMetric]. Mirrors the trace-builder DSL
 * used on the Swift side so the API reads identically from either
 * language.
 */
object Trace {
    /** Single scalar metric — e.g. `Trace.value("opacity", opacity)`. */
    fun value(
        name: String,
        value: Double,
        epsilon: Double = PulpMotion.DEFAULT_EPSILON,
        precision: Int = PulpMotion.DEFAULT_PRECISION,
    ): MotionMetric = MotionMetric.Value(name, value, epsilon, precision)

    /**
     * Geometry metric over the trace's host view — emitted as
     * `(minX, minY, width, height)` by default. Add or remove
     * properties to sample additional axes (`midX`, `maxY`, etc.).
     */
    fun geometry(
        name: String,
        properties: List<MotionGeometryProperty> =
            MotionMetric.Geometry.DEFAULT_PROPERTIES,
        epsilon: Double = MotionMetric.DEFAULT_GEOMETRY_EPSILON,
        precision: Int = MotionMetric.DEFAULT_GEOMETRY_PRECISION,
    ): MotionMetric = MotionMetric.Geometry(name, properties, epsilon, precision)

    /**
     * Convenience for a scroll-container geometry trace — same
     * underlying shape as [geometry] but the name signals intent to
     * readers and downstream analysis tooling.
     */
    fun scrollGeometry(
        name: String,
        epsilon: Double = MotionMetric.DEFAULT_GEOMETRY_EPSILON,
        precision: Int = MotionMetric.DEFAULT_GEOMETRY_PRECISION,
    ): MotionMetric = MotionMetric.ScrollGeometry(name, epsilon, precision)
}

/**
 * Trace-block builder collected by [motionTrace]. Mirrors Swift's
 * `@MotionTraceBuilder` result builder: every static `Trace.*` factory
 * appended in the block body becomes a `MotionMetric` in the returned
 * list.
 */
@MotionTraceBuilder
class MotionTraceScope internal constructor() {
    private val collected = mutableListOf<MotionMetric>()

    operator fun MotionMetric.unaryPlus() { collected += this }

    /** Add a metric to the trace. Most callers will use the `Trace.*`
     *  factories combined with the unary `+` operator. */
    fun metric(metric: MotionMetric) { collected += metric }

    internal fun build(): List<MotionMetric> = collected.toList()
}

/**
 * Top-level entry point for building a list of [MotionMetric]
 * declarations. Used by the View probe (`View.pulpMotionTrace`) and
 * the Compose modifier (`Modifier.pulpMotionGeometry`).
 *
 * ```kotlin
 * val metrics = motionTrace {
 *     +Trace.value("opacity", opacity)
 *     +Trace.geometry("frame")
 * }
 * ```
 */
// Not `inline`: the body calls MotionTraceScope's `internal constructor()`
// and `internal fun build()`, which `inline` would have to expose at call
// sites (compilation error: "Public-API inline function cannot access
// non-public-API constructor/function"). The DSL is invoked once per
// attach, not in a hot loop, so the call overhead is irrelevant.
fun motionTrace(block: MotionTraceScope.() -> Unit): List<MotionMetric> {
    val scope = MotionTraceScope()
    scope.block()
    return scope.build()
}

// ── Rect helpers ─────────────────────────────────────────────────────
//
// Android views deal in `Rect` / `RectF` constantly; expose the
// `publishComponents` shape as an extension so probes don't reinvent
// the (minX, minY, width, height) tuple at every call site.

/**
 * Convert a [RectF] (window-space) to the canonical
 * `(minX, minY, width, height)` component list the C ABI expects.
 * Component names match the Swift bridge so fixtures recorded on
 * Android replay identically against iOS-side analysis tooling.
 */
fun RectF.toMotionComponents(): List<Pair<String, Double>> = listOf(
    "minX"   to left.toDouble(),
    "minY"   to top.toDouble(),
    "width"  to (right - left).toDouble(),
    "height" to (bottom - top).toDouble(),
)

/** [Rect] variant of [toMotionComponents]. */
fun Rect.toMotionComponents(): List<Pair<String, Double>> = listOf(
    "minX"   to left.toDouble(),
    "minY"   to top.toDouble(),
    "width"  to (right - left).toDouble(),
    "height" to (bottom - top).toDouble(),
)
