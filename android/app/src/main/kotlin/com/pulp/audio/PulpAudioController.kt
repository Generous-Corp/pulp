package com.pulp.audio

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.util.Log
import com.pulp.PulpApplication
import com.pulp.PulpAudioService

/**
 * Lifecycle controller for the foreground audio service (#333).
 *
 * Android kills backgrounded apps' audio engines within seconds unless
 * a foreground service is running. PulpAudioService already defines
 * the service + notification; this controller ties its lifecycle to
 * the audio-engine start/stop transitions that the app (or native
 * Oboe code) drives.
 *
 * Usage:
 *
 *     val controller = PulpAudioController(context)
 *     controller.startAudio(isRecording = false)
 *     // ... user leaves app, audio keeps playing ...
 *     controller.stopAudio()
 *
 * The controller is idempotent: redundant starts are no-ops, and a
 * stop without a preceding start is safe. Callers can also consult
 * [isRunning] to drive UI state.
 */
class PulpAudioController(private val context: Context) {

    private var bound = false
    private var service: PulpAudioService? = null
    private var pendingStart: Boolean? = null
    @Volatile private var running = false

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            // PulpAudioService binds itself via LifecycleService default binder
            // (null on connect); we keep the service reference only when the
            // app-level binder is later exposed. For now, use startForeground
            // via an Intent instead of binder calls — safer + matches the
            // FGS-type-required-on-start contract.
            bound = true
            Log.i(PulpApplication.LOG_TAG,
                "PulpAudioController: service connection established")
            pendingStart?.let {
                // No-op — start path uses startForegroundService directly.
                pendingStart = null
            }
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            bound = false
            service = null
            running = false
            Log.i(PulpApplication.LOG_TAG,
                "PulpAudioController: service disconnected")
        }
    }

    /** True when the foreground service is (or has been requested to be) running. */
    val isRunning: Boolean get() = running

    /**
     * Start the audio engine with a foreground service. Pass isRecording = true
     * when capture is active so the FGS type mask includes MICROPHONE (required
     * on Android 14+ when the service reads the mic).
     *
     * Idempotent — a second start() while running is a no-op.
     */
    fun startAudio(isRecording: Boolean = false) {
        if (running) {
            Log.i(PulpApplication.LOG_TAG,
                "PulpAudioController: already running; start() is a no-op")
            return
        }
        val intent = Intent(context, PulpAudioService::class.java)
        intent.putExtra(EXTRA_IS_RECORDING, isRecording)
        intent.action = ACTION_START_FOREGROUND
        // startForegroundService is the Android-8+ API; the service has
        // 5 seconds to call startForeground() from onStartCommand or the
        // system will ANR. PulpAudioService's onStartCommand handler
        // (registered in the same commit) satisfies that contract.
        context.startForegroundService(intent)
        context.bindService(intent, connection, Context.BIND_AUTO_CREATE)
        running = true
        pendingStart = isRecording
        Log.i(PulpApplication.LOG_TAG,
            "PulpAudioController: start requested (recording=$isRecording)")
    }

    /**
     * Stop the audio engine and tear down the foreground service.
     * Idempotent — a stop() when not running is a no-op.
     */
    fun stopAudio() {
        if (!running) return
        running = false
        val intent = Intent(context, PulpAudioService::class.java)
        intent.action = ACTION_STOP_FOREGROUND
        // Tell the service to call stopForeground + stopSelf. We also
        // unbind so the service can be reaped when no other clients
        // hold it.
        context.startService(intent)
        if (bound) {
            try {
                context.unbindService(connection)
            } catch (e: IllegalArgumentException) {
                // Already unbound — fine.
            }
            bound = false
        }
        service = null
        Log.i(PulpApplication.LOG_TAG,
            "PulpAudioController: stop requested")
    }

    companion object {
        const val ACTION_START_FOREGROUND = "com.pulp.action.START_FOREGROUND"
        const val ACTION_STOP_FOREGROUND = "com.pulp.action.STOP_FOREGROUND"
        const val EXTRA_IS_RECORDING = "com.pulp.extra.IS_RECORDING"
    }
}
