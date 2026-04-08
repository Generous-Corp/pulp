package com.pulp

import android.content.ComponentCallbacks2
import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier

class PulpActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(PulpApplication.LOG_TAG, "PulpActivity.onCreate")

        if (PulpApplication.nativeLoaded) nativeOnForeground()

        setContent {
            MaterialTheme {
                Surface(modifier = Modifier.fillMaxSize()) {
                    Box(contentAlignment = Alignment.Center) {
                        Text("Pulp Audio Engine")
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (PulpApplication.nativeLoaded) nativeOnForeground()
    }

    override fun onPause() {
        super.onPause()
        if (PulpApplication.nativeLoaded) nativeOnBackground()
    }

    override fun onDestroy() {
        if (PulpApplication.nativeLoaded) nativeOnShutdown()
        super.onDestroy()
    }

    // configChanges in manifest prevents Activity destruction on rotation/fold.
    // This callback handles the display change without tearing the Vulkan surface.
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)

        val displayMetrics = resources.displayMetrics
        val darkMode = newConfig.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
                Configuration.UI_MODE_NIGHT_YES

        nativeOnDisplayChanged(
            displayMetrics.widthPixels,
            displayMetrics.heightPixels,
            displayMetrics.density,
            darkMode
        )
    }

    // Low Memory Killer survival — tiered cache release
    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)

        val pressureLevel = when (level) {
            ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN -> 0  // moderate
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW,
            ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL -> 1  // aggressive
            ComponentCallbacks2.TRIM_MEMORY_BACKGROUND,
            ComponentCallbacks2.TRIM_MEMORY_MODERATE,
            ComponentCallbacks2.TRIM_MEMORY_COMPLETE -> 2  // emergency
            else -> return
        }

        nativeOnMemoryPressure(pressureLevel)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        // TODO: nativeSerializeState() → outState.putByteArray("pulp_state", state)
    }

    override fun onRestoreInstanceState(savedInstanceState: Bundle) {
        super.onRestoreInstanceState(savedInstanceState)
        // TODO: savedInstanceState.getByteArray("pulp_state")?.let { nativeRestoreState(it) }
    }

    // Native methods — implemented in jni_bridge.cpp
    private external fun nativeOnForeground()
    private external fun nativeOnBackground()
    private external fun nativeOnShutdown()
    private external fun nativeOnMemoryPressure(level: Int)
    private external fun nativeOnDisplayChanged(width: Int, height: Int, density: Float, darkMode: Boolean)
    external fun nativeOnPermissionResult(permission: Int, granted: Boolean)
}
