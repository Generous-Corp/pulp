package com.pulp.motion

import android.graphics.Rect
import android.graphics.RectF
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test

/**
 * JVM unit tests for the Kotlin motion facade.
 *
 * These tests deliberately exercise the closure-bag backend (no JNI,
 * no `System.loadLibrary`) so they run on a vanilla `gradle test`
 * without an emulator and without Robolectric. Mirrors the testability
 * pattern Swift uses for `PulpMotionRuntime.installTestBackend(...)`.
 */
class PulpMotionTest {

    /** Records every call the facade routes through the backend. */
    private class RecorderBackend(var tracingEnabled: Boolean = true) {
        data class PublishValueCall(
            val view: String,
            val metric: String,
            val value: Double,
            val epsilon: Double,
            val precision: Int,
        )

        data class PublishComponentsCall(
            val view: String,
            val metric: String,
            val components: List<Pair<String, Double>>,
            val epsilon: Double,
            val precision: Int,
        )

        data class ProvenanceCall(
            val kind: String,
            val id: String,
            val file: String,
            val line: Int,
        )

        data class GeometryUpdate(
            val traceId: Int,
            val metric: String,
            val minX: Double,
            val minY: Double,
            val width: Double,
            val height: Double,
        )

        val publishedValues = mutableListOf<PublishValueCall>()
        val publishedComponents = mutableListOf<PublishComponentsCall>()
        val ambientStack = mutableListOf<ProvenanceCall?>()
        val registered = mutableListOf<Pair<String, Int>>()
        val updates = mutableListOf<GeometryUpdate>()
        val detached = mutableListOf<Int>()
        var nextTraceId: Int = 100

        fun asBackend(): PulpMotionBackend = PulpMotionBackend(
            isTracingEnabled = { tracingEnabled },
            publishValue = { v, m, value, eps, pr ->
                publishedValues += PublishValueCall(v, m, value, eps, pr)
            },
            publishComponents = { v, m, comps, eps, pr ->
                publishedComponents += PublishComponentsCall(v, m, comps, eps, pr)
            },
            setAmbientProvenance = { kind, id, file, line ->
                ambientStack += ProvenanceCall(kind, id, file, line)
            },
            clearAmbientProvenance = { ambientStack += null },
            registerGeometryTrace = { v, fps ->
                val id = nextTraceId++
                registered += v to fps
                id
            },
            updateGeometry = { id, m, x, y, w, h ->
                updates += GeometryUpdate(id, m, x, y, w, h)
            },
            detachTrace = { id -> detached += id },
        )
    }

    private lateinit var recorder: RecorderBackend

    @Before
    fun setUp() {
        recorder = RecorderBackend(tracingEnabled = true)
        PulpMotionRuntime.installBackend(recorder.asBackend())
    }

    @After
    fun tearDown() {
        // Reset for the next test class — also exercises the
        // "null reverts to no-op" branch of installBackend.
        PulpMotionRuntime.installBackend(null)
    }

    // ── Off-by-default ───────────────────────────────────────────────

    @Test
    fun publishValueShortCircuitsWhenTracingDisabled() {
        recorder.tracingEnabled = false

        PulpMotion.publishValue(view = "Card", metric = "opacity", value = 0.5)
        PulpMotion.publishComponents(
            view = "Card",
            metric = "frame",
            components = listOf("x" to 1.0, "y" to 2.0),
        )

        assertTrue(
            "publishValue should not reach the backend when tracing is off",
            recorder.publishedValues.isEmpty(),
        )
        assertTrue(
            "publishComponents should not reach the backend when tracing is off",
            recorder.publishedComponents.isEmpty(),
        )
        assertFalse(PulpMotion.isTracingEnabled)
    }

    @Test
    fun publishValueRoutesThroughBackendWhenEnabled() {
        PulpMotion.publishValue(
            view = "Card",
            metric = "opacity",
            value = 0.5,
            epsilon = 0.01,
            precision = 4,
        )

        assertEquals(1, recorder.publishedValues.size)
        val call = recorder.publishedValues.single()
        assertEquals("Card", call.view)
        assertEquals("opacity", call.metric)
        assertEquals(0.5, call.value, 0.0)
        assertEquals(0.01, call.epsilon, 0.0)
        assertEquals(4, call.precision)
    }

    // ── withProvenance lifecycle ────────────────────────────────────

    @Test
    fun withProvenanceClearsEvenWhenBlockThrows() {
        try {
            PulpMotion.withProvenance(kind = "android", id = "CardView") {
                throw IllegalStateException("boom")
            }
            fail("withProvenance should rethrow the block's exception")
        } catch (_: IllegalStateException) {
            // expected
        }

        // ambientStack receives: [set, clear]. The set call carries
        // our envelope; the clear call is null.
        assertEquals(2, recorder.ambientStack.size)
        val set = recorder.ambientStack[0]
        assertNotNull(set)
        assertEquals("android", set!!.kind)
        assertEquals("CardView", set.id)
        assertNull("clear must follow the set even on throw", recorder.ambientStack[1])
    }

    @Test
    fun withProvenanceReturnsBlockResult() {
        val result = PulpMotion.withProvenance(kind = "android", id = "x") {
            42
        }
        assertEquals(42, result)
        assertEquals(2, recorder.ambientStack.size)
    }

    @Test
    fun setAndClearAmbientProvenanceRouteIndividually() {
        PulpMotion.setAmbientProvenance(
            kind = "android",
            id = "CardView",
            file = "MotionProbe.kt",
            line = 99,
        )
        PulpMotion.clearAmbientProvenance()

        assertEquals(2, recorder.ambientStack.size)
        val set = recorder.ambientStack.first()
        assertNotNull(set)
        assertEquals("MotionProbe.kt", set!!.file)
        assertEquals(99, set.line)
        assertNull(recorder.ambientStack.last())
    }

    // ── Trace DSL ───────────────────────────────────────────────────

    @Test
    fun traceGeometryDefaultsToMinXMinYWidthHeight() {
        val metric = Trace.geometry("frame")
        assertTrue(metric is MotionMetric.Geometry)
        val g = metric as MotionMetric.Geometry
        assertEquals(
            listOf(
                MotionGeometryProperty.minX,
                MotionGeometryProperty.minY,
                MotionGeometryProperty.width,
                MotionGeometryProperty.height,
            ),
            g.properties,
        )
        assertEquals(MotionMetric.DEFAULT_GEOMETRY_EPSILON, g.epsilon, 0.0)
        assertEquals(MotionMetric.DEFAULT_GEOMETRY_PRECISION, g.precision)
    }

    @Test
    fun traceValueCarriesEpsilonAndPrecision() {
        val metric = Trace.value("opacity", 0.75, epsilon = 0.05, precision = 2)
        assertTrue(metric is MotionMetric.Value)
        val v = metric as MotionMetric.Value
        assertEquals(0.75, v.value, 0.0)
        assertEquals(0.05, v.epsilon, 0.0)
        assertEquals(2, v.precision)
    }

    @Test
    fun motionTraceBuilderCollectsMetricsInOrder() {
        val collected = motionTrace {
            +Trace.value("opacity", 1.0)
            +Trace.geometry("frame")
            +Trace.scrollGeometry("scroll")
        }
        assertEquals(3, collected.size)
        assertEquals("opacity", collected[0].name)
        assertEquals("frame", collected[1].name)
        assertEquals("scroll", collected[2].name)
    }

    // ── Rect helpers ────────────────────────────────────────────────

    private fun rectF(left: Float, top: Float, right: Float, bottom: Float): RectF =
        RectF().apply {
            this.left = left
            this.top = top
            this.right = right
            this.bottom = bottom
        }

    private fun rect(left: Int, top: Int, right: Int, bottom: Int): Rect =
        Rect().apply {
            this.left = left
            this.top = top
            this.right = right
            this.bottom = bottom
        }

    @Test
    fun rectFToMotionComponentsMatchesCanonicalShape() {
        val r = rectF(10f, 20f, 110f, 220f)  // width=100, height=200
        val comps = r.toMotionComponents()
        assertEquals(
            listOf(
                "minX" to 10.0,
                "minY" to 20.0,
                "width" to 100.0,
                "height" to 200.0,
            ),
            comps,
        )
    }

    @Test
    fun rectToMotionComponentsMatchesCanonicalShape() {
        val r = rect(5, 7, 25, 47)  // width=20, height=40
        val comps = r.toMotionComponents()
        assertEquals(
            listOf(
                "minX" to 5.0,
                "minY" to 7.0,
                "width" to 20.0,
                "height" to 40.0,
            ),
            comps,
        )
    }

    @Test
    fun publishComponentsForwardsRectShapeToBackend() {
        val r = rectF(0f, 0f, 320f, 480f)
        PulpMotion.publishComponents(
            view = "ScrollView",
            metric = "frame",
            components = r.toMotionComponents(),
        )

        assertEquals(1, recorder.publishedComponents.size)
        val call = recorder.publishedComponents.single()
        assertEquals("ScrollView", call.view)
        assertEquals("frame", call.metric)
        assertEquals(4, call.components.size)
        assertEquals("minX", call.components[0].first)
        assertEquals("minY", call.components[1].first)
        assertEquals("width", call.components[2].first)
        assertEquals("height", call.components[3].first)
        assertEquals(320.0, call.components[2].second, 0.0)
        assertEquals(480.0, call.components[3].second, 0.0)
    }

    // ── Probe lifecycle (no Android View — exercise the facade) ─────

    @Test
    fun registerUpdateDetachCallsThreadIdThroughBackend() {
        // Mimic the call sequence MotionProbe.kt issues — register
        // returns the recorder's incrementing id, update + detach
        // forward it untouched.
        val id = PulpMotion.registerGeometryTrace("Card", fps = 60)
        assertEquals(100, id)
        PulpMotion.updateGeometry(
            traceId = id,
            metricName = "frame",
            minX = 0.0, minY = 0.0,
            width = 100.0, height = 50.0,
        )
        PulpMotion.detachTrace(id)

        assertEquals(listOf("Card" to 60), recorder.registered)
        assertEquals(1, recorder.updates.size)
        assertEquals(id, recorder.updates.single().traceId)
        assertEquals("frame", recorder.updates.single().metric)
        assertEquals(listOf(id), recorder.detached)
    }

    @Test
    fun updateGeometryNoOpsWhenTraceIdIsZero() {
        // The View probe returns null (and so never calls update) when
        // register returns 0; the facade also guards this directly so
        // direct callers can't spam updates with a sentinel id.
        PulpMotion.updateGeometry(
            traceId = 0,
            metricName = "frame",
            minX = 0.0, minY = 0.0,
            width = 100.0, height = 50.0,
        )
        PulpMotion.detachTrace(0)
        assertTrue(recorder.updates.isEmpty())
        assertTrue(recorder.detached.isEmpty())
    }

    @Test
    fun installNullBackendRevertsToNoOpDefault() {
        PulpMotionRuntime.installBackend(null)
        assertFalse(PulpMotion.isTracingEnabled)
        // No throw — every entry point falls through to the noOp closures.
        PulpMotion.publishValue("v", "m", 0.0)
        PulpMotion.publishComponents("v", "m", listOf("a" to 1.0))
        PulpMotion.setAmbientProvenance("k", "i")
        PulpMotion.clearAmbientProvenance()
        assertEquals(0, PulpMotion.registerGeometryTrace("v"))
    }
}
