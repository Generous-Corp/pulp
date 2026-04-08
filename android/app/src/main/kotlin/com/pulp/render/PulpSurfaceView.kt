package com.pulp.render

import android.content.Context
import android.util.Log
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
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
        // Render above the Compose window layer so Vulkan content is visible
        setZOrderOnTop(true)
        holder.setFormat(android.graphics.PixelFormat.TRANSLUCENT)
    }

    // ── Surface Lifecycle ─────────────────────────────────────────────────

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceCreated")
        if (PulpApplication.nativeLoaded) {
            nativeOnSurfaceCreated(holder.surface)
        }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surfaceChanged: ${width}x${height} format=$format")
        if (PulpApplication.nativeLoaded) {
            nativeOnSurfaceResized(width, height)
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        // CRITICAL: This must block until the C++ render thread has fully stopped.
        // Returning before the render thread releases the surface → SIGSEGV.
        Log.i(TAG, "surfaceDestroyed: waiting for render thread to stop...")
        if (PulpApplication.nativeLoaded) {
            nativeOnSurfaceDestroyed()  // synchronous — uses condition_variable
        }
        Log.i(TAG, "surfaceDestroyed: render thread confirmed stopped")
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
