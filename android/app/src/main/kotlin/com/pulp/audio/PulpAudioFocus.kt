package com.pulp.audio

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.util.Log
import com.pulp.PulpApplication

/**
 * Audio focus management for Android.
 * Handles ducking on transient interruptions (phone calls, navigation),
 * pausing on full focus loss, and resuming on focus gain.
 */
class PulpAudioFocus(context: Context) {

    private val audioManager = context.getSystemService(AudioManager::class.java)
    private var hasAudioFocus = false

    private val focusRequest = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
        .setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build()
        )
        .setOnAudioFocusChangeListener { focusChange ->
            when (focusChange) {
                AudioManager.AUDIOFOCUS_LOSS -> {
                    Log.i(TAG, "Audio focus lost — pausing")
                    hasAudioFocus = false
                    if (PulpApplication.nativeLoaded) nativeOnAudioFocusLost()
                }
                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                    // Non-ducking transient loss: the listener should pause
                    // output (phone call, navigation prompt). Distinct from
                    // LOSS_TRANSIENT_CAN_DUCK, which just attenuates.
                    // Route to the dedicated JNI entry so subscribers can
                    // distinguish pause vs duck.
                    Log.i(TAG, "Audio focus lost transiently — pausing")
                    // Mirror the LOSS path and clear hasAudioFocus so callers
                    // checking hasFocus() during the transient don't see stale
                    // `true` and keep pushing audio.
                    hasAudioFocus = false
                    if (PulpApplication.nativeLoaded) nativeOnAudioFocusLostTransient()
                }
                AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                    Log.i(TAG, "Audio focus can duck")
                    if (PulpApplication.nativeLoaded) nativeOnAudioFocusDuck()
                }
                AudioManager.AUDIOFOCUS_GAIN -> {
                    Log.i(TAG, "Audio focus gained — resuming")
                    hasAudioFocus = true
                    if (PulpApplication.nativeLoaded) nativeOnAudioFocusGained()
                }
            }
        }
        .build()

    fun requestFocus(): Boolean {
        val result = audioManager.requestAudioFocus(focusRequest)
        hasAudioFocus = (result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED)
        Log.i(TAG, "Audio focus request: ${if (hasAudioFocus) "granted" else "denied"}")
        return hasAudioFocus
    }

    fun abandonFocus() {
        audioManager.abandonAudioFocusRequest(focusRequest)
        hasAudioFocus = false
        Log.i(TAG, "Audio focus abandoned")
    }

    fun hasFocus(): Boolean = hasAudioFocus

    private external fun nativeOnAudioFocusLost()
    private external fun nativeOnAudioFocusLostTransient()
    private external fun nativeOnAudioFocusDuck()
    private external fun nativeOnAudioFocusGained()

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
    }
}
