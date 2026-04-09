package com.pulp

import android.app.NotificationChannel
import android.app.NotificationManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.lifecycle.LifecycleService

/**
 * Foreground service for background audio processing.
 * Without this, Android will kill the app's audio engine within seconds of backgrounding.
 */
class PulpAudioService : LifecycleService() {

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
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
