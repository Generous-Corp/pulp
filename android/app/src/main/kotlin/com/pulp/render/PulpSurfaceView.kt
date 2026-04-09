package com.pulp.render

import android.content.Context
import android.util.Log
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.pulp.PulpApplication

/**
 * SurfaceView that hosts the Vulkan/Dawn rendering surface.
 *
 * Key lifecycle contract:
 * - surfaceCreated: pass ANativeWindow to C++ for Dawn/Vulkan surface creation
 * - surfaceDestroyed: SYNCHRONOUSLY block until C++ render thread confirms stop
 *   (returning early while C++ still renders → SIGSEGV)
 * - Touch events dispatched to C++ view hierarchy
 */
class PulpSurfaceView(context: Context) : SurfaceView(context), SurfaceHolder.Callback {

    init {
        holder.addCallback(this)
        // Pass real display density to C++ before surface is created
        if (PulpApplication.nativeLoaded) {
            val density = resources.displayMetrics.density
            Log.i(TAG, "Setting display density: $density")
            nativeSetDisplayDensity(density)
        }

        // Pass system bar insets (status bar, nav bar) to C++ for safe area padding
        ViewCompat.setOnApplyWindowInsetsListener(this) { _, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            val density = resources.displayMetrics.density
            // Convert px insets to dp for the C++ view system
            if (PulpApplication.nativeLoaded) {
                nativeSetSafeAreaInsets(
                    bars.top / density,
                    bars.bottom / density,
                    bars.left / density,
                    bars.right / density
                )
                Log.i(TAG, "Safe area insets (dp): top=${bars.top/density} bottom=${bars.bottom/density}")
            }
            insets
        }
    }

    // ── Surface Lifecycle ─────────────────────────────────────────────────

    private var renderThread: Thread? = null

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceCreated — launching GPU init on render thread")
        if (PulpApplication.nativeLoaded) {
            // Run Dawn/Skia initialization on a dedicated thread to avoid ANR.
            // Dawn shader compilation takes 10-15 seconds on the emulator.
            // The render thread becomes the owner of the GPU context and
            // AChoreographer render loop.
            initComplete = false
            renderThread = Thread({
                Log.i(TAG, "Render thread started")
                android.os.Looper.prepare()  // AChoreographer needs a Looper
                renderLooper = android.os.Looper.myLooper()
                nativeOnSurfaceCreated(holder.surface)
                initComplete = true
                Log.i(TAG, "Dawn init complete, entering Looper for choreographer callbacks")
                android.os.Looper.loop()     // Blocks — processes AChoreographer callbacks
                Log.i(TAG, "Render thread Looper exited")
            }, "PulpRenderThread").also { it.start() }
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surfaceChanged: ${width}x${height} format=$format")
        if (PulpApplication.nativeLoaded) {
            nativeOnSurfaceResized(width, height)
        }
    }

    @Volatile private var renderLooper: android.os.Looper? = null
    @Volatile private var initComplete = false

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceDestroyed (initComplete=$initComplete)")
        if (!initComplete) {
            // Dawn is still initializing on the render thread.
            // Don't block — just let the render thread finish and discover
            // the surface is gone on its next frame attempt.
            Log.i(TAG, "surfaceDestroyed during init — not blocking")
            return
        }
        if (PulpApplication.nativeLoaded) {
            nativeOnSurfaceDestroyed()
        }
        renderLooper?.quitSafely()
        renderThread?.let { thread ->
            try {
                thread.join(5000)
            } catch (e: InterruptedException) {
                Log.w(TAG, "Interrupted waiting for render thread")
            }
            renderThread = null
            renderLooper = null
        }
        initComplete = false
        Log.i(TAG, "surfaceDestroyed: render thread stopped")
    }

    // ── Touch Input ───────────────────────────────────────────────────────

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!PulpApplication.nativeLoaded) return super.onTouchEvent(event)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val idx = event.actionIndex
                nativeOnTouchDown(
                    event.getPointerId(idx),
                    event.getX(idx), event.getY(idx),
                    event.getPressure(idx)
                )
            }
            MotionEvent.ACTION_MOVE -> {
                for (i in 0 until event.pointerCount) {
                    nativeOnTouchMove(
                        event.getPointerId(i),
                        event.getX(i), event.getY(i),
                        event.getPressure(i)
                    )
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
                val idx = event.actionIndex
                nativeOnTouchUp(
                    event.getPointerId(idx),
                    event.getX(idx), event.getY(idx)
                )
            }
            MotionEvent.ACTION_CANCEL -> {
                nativeOnTouchCancel()
            }
        }
        return true
    }

    // ── Native Methods ────────────────────────────────────────────────────

    // Display density — called once in init, before surface lifecycle
    private external fun nativeSetDisplayDensity(density: Float)
    // Safe area insets (dp) — status bar, nav bar, notch
    private external fun nativeSetSafeAreaInsets(top: Float, bottom: Float, left: Float, right: Float)

    // Surface lifecycle — called on main thread
    private external fun nativeOnSurfaceCreated(surface: Surface)
    private external fun nativeOnSurfaceResized(width: Int, height: Int)
    private external fun nativeOnSurfaceDestroyed()  // blocks until render thread stops

    // Touch events — called on main thread
    private external fun nativeOnTouchDown(pointerId: Int, x: Float, y: Float, pressure: Float)
    private external fun nativeOnTouchMove(pointerId: Int, x: Float, y: Float, pressure: Float)
    private external fun nativeOnTouchUp(pointerId: Int, x: Float, y: Float)
    private external fun nativeOnTouchCancel()

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
    }
}
