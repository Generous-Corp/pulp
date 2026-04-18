package com.pulp

import android.content.ComponentCallbacks2
import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
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
            val initialOrientation = resources.configuration.orientation
            nativeOnOrientationChanged(orientationToEnum(initialOrientation))

            ViewCompat.setOnApplyWindowInsetsListener(frame) { _, insets ->
                val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
                val ime  = insets.getInsets(WindowInsetsCompat.Type.ime())
                if (PulpApplication.nativeLoaded) {
                    nativeOnSafeAreaChanged(
                        bars.top.toFloat(),
                        bars.bottom.toFloat(),
                        bars.left.toFloat(),
                        bars.right.toFloat()
                    )
                    // Keyboard is considered visible when IME height
                    // exceeds the system bars' bottom inset; subtract
                    // so callers see the "additional" bottom padding
                    // the keyboard introduced, not the full IME height
                    // (which already includes the system nav bar).
                    val keyboardBottom = maxOf(0, ime.bottom - bars.bottom).toFloat()
                    nativeOnKeyboardChanged(keyboardBottom)
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
            nativeOnOrientationChanged(orientationToEnum(newConfig.orientation))
        }
    }

    // Map Android's Configuration.ORIENTATION_* to the Pulp C++
    // Orientation enum values (declared in environment.hpp). Kept
    // side-by-side with the C++ to keep the mapping obvious.
    private fun orientationToEnum(androidOrientation: Int): Int = when (androidOrientation) {
        Configuration.ORIENTATION_PORTRAIT  -> 0   // Orientation::portrait
        Configuration.ORIENTATION_LANDSCAPE -> 2   // Orientation::landscape_left
        else                                -> 5   // Orientation::unknown
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
