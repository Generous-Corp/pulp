package com.pulp

import android.content.ComponentCallbacks2
import android.content.res.Configuration
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.ui.viewinterop.AndroidView
import com.pulp.render.PulpSurfaceView
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView

class PulpActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i(PulpApplication.LOG_TAG, "PulpActivity.onCreate")

        if (PulpApplication.nativeLoaded) nativeOnForeground()

        setContent {
            PulpDemoApp()
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
        if (PulpApplication.nativeLoaded) {
            nativeStopTone()
            nativeOnShutdown()
        }
        super.onDestroy()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        val dm = resources.displayMetrics
        val dark = newConfig.uiMode and Configuration.UI_MODE_NIGHT_MASK ==
                Configuration.UI_MODE_NIGHT_YES
        if (PulpApplication.nativeLoaded)
            nativeOnDisplayChanged(dm.widthPixels, dm.heightPixels, dm.density, dark)
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

    // Native methods
    private external fun nativeOnForeground()
    private external fun nativeOnBackground()
    private external fun nativeOnShutdown()
    private external fun nativeOnMemoryPressure(level: Int)
    private external fun nativeOnDisplayChanged(w: Int, h: Int, density: Float, dark: Boolean)
    external fun nativeOnPermissionResult(permission: Int, granted: Boolean)
}

// ── Tone generator native interface ───────────────────────────────────────
// These are thin JNI wrappers around the Oboe audio engine.

private external fun nativeStartTone(frequencyHz: Float)
private external fun nativeStopTone()
private external fun nativeSetFrequency(frequencyHz: Float)
private external fun nativeIsPlaying(): Boolean
private external fun nativeGetSampleRate(): Int
private external fun nativeGetBufferSize(): Int
private external fun nativeGetXrunCount(): Long

@Composable
fun PulpDemoApp() {
    var isPlaying by remember { mutableStateOf(false) }
    var frequency by remember { mutableFloatStateOf(440f) }
    var sampleRate by remember { mutableIntStateOf(0) }
    var bufferSize by remember { mutableIntStateOf(0) }
    var xruns by remember { mutableLongStateOf(0L) }
    val nativeLoaded = PulpApplication.nativeLoaded

    MaterialTheme(
        colorScheme = darkColorScheme()
    ) {
        Surface(
            modifier = Modifier.fillMaxSize(),
            color = MaterialTheme.colorScheme.background
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(24.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Spacer(Modifier.height(40.dp))

                // Title
                Text(
                    "Pulp Audio Engine",
                    fontSize = 28.sp,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.primary
                )
                Text(
                    "Android Demo",
                    fontSize = 16.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )

                Spacer(Modifier.height(24.dp))

                // Status card
                Card(
                    modifier = Modifier.fillMaxWidth(),
                    colors = CardDefaults.cardColors(
                        containerColor = MaterialTheme.colorScheme.surfaceVariant
                    )
                ) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text("Engine Status", fontWeight = FontWeight.SemiBold,
                            color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Spacer(Modifier.height(8.dp))
                        StatusRow("Native Library", if (nativeLoaded) "Loaded" else "Not loaded")
                        StatusRow("Sample Rate", if (sampleRate > 0) "$sampleRate Hz" else "—")
                        StatusRow("Buffer Size", if (bufferSize > 0) "$bufferSize frames" else "—")
                        StatusRow("Xruns", "$xruns")
                        StatusRow("Audio", if (isPlaying) "Playing" else "Stopped")
                    }
                }

                Spacer(Modifier.height(16.dp))

                // Frequency slider
                Text(
                    "Frequency: ${frequency.toInt()} Hz",
                    fontSize = 18.sp,
                    color = MaterialTheme.colorScheme.onSurface
                )
                Slider(
                    value = frequency,
                    onValueChange = { freq ->
                        frequency = freq
                        if (nativeLoaded && isPlaying) {
                            nativeSetFrequency(freq)
                        }
                    },
                    valueRange = 110f..880f,
                    modifier = Modifier.fillMaxWidth()
                )

                Spacer(Modifier.height(16.dp))

                // Play/Stop button
                Button(
                    onClick = {
                        if (!nativeLoaded) return@Button
                        if (isPlaying) {
                            nativeStopTone()
                            isPlaying = false
                        } else {
                            nativeStartTone(frequency)
                            isPlaying = true
                            sampleRate = nativeGetSampleRate()
                            bufferSize = nativeGetBufferSize()
                        }
                        xruns = if (nativeLoaded) nativeGetXrunCount() else 0L
                    },
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(56.dp),
                    colors = ButtonDefaults.buttonColors(
                        containerColor = if (isPlaying)
                            MaterialTheme.colorScheme.error
                        else
                            MaterialTheme.colorScheme.primary
                    ),
                    enabled = nativeLoaded
                ) {
                    Text(
                        if (isPlaying) "Stop" else "Play Tone",
                        fontSize = 18.sp
                    )
                }

                if (!nativeLoaded) {
                    Text(
                        "Native library not loaded",
                        color = MaterialTheme.colorScheme.error,
                        fontSize = 14.sp
                    )
                }

                // Vulkan/Dawn rendering surface
                if (nativeLoaded) {
                    Spacer(Modifier.height(16.dp))
                    Text(
                        "GPU Surface",
                        fontSize = 16.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                    Card(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(200.dp),
                        colors = CardDefaults.cardColors(
                            containerColor = MaterialTheme.colorScheme.surfaceVariant
                        )
                    ) {
                        AndroidView(
                            factory = { context -> PulpSurfaceView(context) },
                            modifier = Modifier.fillMaxSize()
                        )
                    }
                }
            }
        }
    }
}

@Composable
fun StatusRow(label: String, value: String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value, fontWeight = FontWeight.Medium,
            color = MaterialTheme.colorScheme.onSurface)
    }
}
