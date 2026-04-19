package com.pulp

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.lifecycle.LifecycleService
import com.pulp.audio.PulpAudioController

/**
 * Foreground service for background audio processing (#333).
 *
 * Android kills backgrounded apps' audio engines within seconds unless
 * a foreground service is running. PulpAudioController drives this
 * service's lifecycle via two intent actions:
 *
 *   ACTION_START_FOREGROUND (+ EXTRA_IS_RECORDING) — promote to FGS
 *   ACTION_STOP_FOREGROUND — demote and stopSelf()
 *
 * Callers don't need to bind; startService / startForegroundService
 * with the action is enough.
 */
class PulpAudioService : LifecycleService() {

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        super.onStartCommand(intent, flags, startId)
        when (intent?.action) {
            PulpAudioController.ACTION_START_FOREGROUND -> {
                val recording = intent.getBooleanExtra(
                    PulpAudioController.EXTRA_IS_RECORDING, false)
                // Must call startForeground within 5 seconds of
                // startForegroundService or the system ANRs the process.
                startAudioForeground(recording)
            }
            PulpAudioController.ACTION_STOP_FOREGROUND -> {
                stopAudioForeground()
                stopSelf(startId)
            }
            else -> {
                // Legacy / unscoped start — default to playback so the
                // 5-second foreground deadline is always met.
                startAudioForeground(isRecording = false)
            }
        }
        // START_STICKY so the service is restarted after an OOM kill —
        // the controller can re-bind and resume. START_NOT_STICKY would
        // leave the user silently without audio.
        return START_STICKY
    }

    fun startAudioForeground(isRecording: Boolean) {
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Pulp Audio")
            .setContentText(if (isRecording) "Recording..." else "Playing...")
            .setSmallIcon(android.R.drawable.ic_media_play)
            .setOngoing(true)
            .build()

        val foregroundServiceType = if (isRecording) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE or
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
        } else {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
        }

        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION_ID, notification, foregroundServiceType)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        Log.i(PulpApplication.LOG_TAG, "Audio foreground service started (recording=$isRecording)")
    }

    fun stopAudioForeground() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        Log.i(PulpApplication.LOG_TAG, "Audio foreground service stopped")
    }

    private fun createNotificationChannel() {
        val channel = NotificationChannel(
            CHANNEL_ID,
            "Audio Engine",
            NotificationManager.IMPORTANCE_LOW  // no sound, just persistent notification
        )
        getSystemService(NotificationManager::class.java)
            .createNotificationChannel(channel)
    }

    companion object {
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "pulp_audio"
    }
}
