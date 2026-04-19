package com.pulp

import android.content.ComponentCallbacks2
import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
import android.view.Surface
import android.view.View
import android.view.WindowInsets
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.pulp.audio.PulpAudioFocus
import com.pulp.render.PulpSurfaceView

class PulpActivity : ComponentActivity() {

    private lateinit var audioFocus: PulpAudioFocus

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(PulpApplication.LOG_TAG, "PulpActivity.onCreate")

        audioFocus = PulpAudioFocus(this)

        if (PulpApplication.nativeLoaded) nativeOnForeground()

        // Fullscreen Pulp rendering surface
        val surfaceView = PulpSurfaceView(this)
        val frame = FrameLayout(this)
        frame.addView(surfaceView)
        setContentView(frame)

        // Publish initial orientation + safe-area + keyboard insets as
        // soon as the decor view has window insets. Also subscribe to
        // subsequent updates (keyboard show/hide, notch enter/exit on
        // rotation, split-screen insets). Forwards into the C++
        // Environment API (#342).
        if (PulpApplication.nativeLoaded) {
            nativeOnOrientationChanged(orientationToEnum())

            ViewCompat.setOnApplyWindowInsetsListener(frame) { _, insets ->
                val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
                val ime  = insets.getInsets(WindowInsetsCompat.Type.ime())
                if (PulpApplication.nativeLoaded) {
                    // EnvironmentState documents safe_area in CSS-pixel
                    // space (already divided by content scale). Android's
                    // WindowInsetsCompat returns physical pixels — divide
                    // by display density so high-DPI devices don't get
                    // 3x oversized insets. See #438 P2 Codex review on
                    // #443.
                    val density = resources.displayMetrics.density
                    nativeOnSafeAreaChanged(
                        bars.top    / density,
                        bars.bottom / density,
                        bars.left   / density,
                        bars.right  / density
                    )
                    // Keyboard is considered visible when IME height
                    // exceeds the system bars' bottom inset; subtract
                    // so callers see the "additional" bottom padding
                    // the keyboard introduced, not the full IME height
                    // (which already includes the system nav bar).
                    val keyboardBottomPx = maxOf(0, ime.bottom - bars.bottom)
                    nativeOnKeyboardChanged(keyboardBottomPx / density)
                }
                insets
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Request audio focus so the system doesn't mute our Oboe stream
        audioFocus.requestFocus()
        if (PulpApplication.nativeLoaded) nativeOnForeground()
    }

    override fun onPause() {
        super.onPause()
        // Don't abandon audio focus on pause — the Activity gets briefly paused
        // during surface transitions and we don't want to kill audio each time.
        if (PulpApplication.nativeLoaded) nativeOnBackground()
    }

    override fun onDestroy() {
        audioFocus.abandonFocus()
        if (PulpApplication.nativeLoaded) nativeOnShutdown()
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val dm = resources.displayMetrics
        val dark = newConfig.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
                Configuration.UI_MODE_NIGHT_YES
        if (PulpApplication.nativeLoaded) {
            nativeOnDisplayChanged(dm.widthPixels, dm.heightPixels, dm.density, dark)
            nativeOnOrientationChanged(orientationToEnum())
        }
    }

    // Map current display rotation to the Pulp C++ Orientation enum
    // values (declared in environment.hpp). Configuration.ORIENTATION_*
    // collapses both landscape sides into LANDSCAPE; we use
    // display.rotation to recover the side. See #438 P2 Codex review
    // on #443. Enum values:
    //   0 portrait, 1 portrait_upside_down,
    //   2 landscape_left, 3 landscape_right,
    //   4 flat, 5 unknown
    private fun orientationToEnum(): Int {
        val rotation = if (android.os.Build.VERSION.SDK_INT >=
                           android.os.Build.VERSION_CODES.R) {
            display?.rotation ?: Surface.ROTATION_0
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.rotation
        }
        return when (rotation) {
            Surface.ROTATION_0   -> 0  // portrait
            Surface.ROTATION_90  -> 2  // landscape_left  (device rotated CCW)
            Surface.ROTATION_180 -> 1  // portrait_upside_down
            Surface.ROTATION_270 -> 3  // landscape_right (device rotated CW)
            else                 -> 5  // unknown
        }
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        val pressureLevel = when (level) {
            ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN -> 0
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW,
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL -> 1
            ComponentCallbacks2.TRIM_MEMORY_BACKGROUND,
            ComponentCallbacks2.TRIM_MEMORY_MODERATE,
            ComponentCallbacks2.TRIM_MEMORY_COMPLETE -> 2
            else -> return
        }
        if (PulpApplication.nativeLoaded) nativeOnMemoryPressure(pressureLevel)
    }

    private external fun nativeOnForeground()
    private external fun nativeOnBackground()
    private external fun nativeOnShutdown()
    private external fun nativeOnMemoryPressure(level: Int)
    private external fun nativeOnDisplayChanged(w: Int, h: Int, density: Float, dark: Boolean)
    private external fun nativeOnOrientationChanged(orientation: Int)
    private external fun nativeOnSafeAreaChanged(top: Float, bottom: Float, left: Float, right: Float)
    private external fun nativeOnKeyboardChanged(bottom: Float)
    external fun nativeOnPermissionResult(permission: Int, granted: Boolean)
}
