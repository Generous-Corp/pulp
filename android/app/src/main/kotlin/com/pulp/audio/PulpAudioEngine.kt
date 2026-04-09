package com.pulp.audio

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.util.Log
import com.pulp.PulpApplication

/**
 * Kotlin-side audio engine management.
 * Wraps AudioManager for device enumeration, optimal config queries,
 * and device change callbacks. The actual audio processing happens in
 * C++ via Oboe — this class provides the JNI-accessible metadata.
 */
class PulpAudioEngine(private val context: Context) {

    private val audioManager = context.getSystemService(AudioManager::class.java)

    fun enumerateDevices(): List<AudioDeviceInfo> {
        return audioManager.getDevices(
            AudioManager.GET_DEVICES_INPUTS or AudioManager.GET_DEVICES_OUTPUTS
        ).toList()
    }

    fun getOptimalSampleRate(): Int {
        return audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)
            ?.toIntOrNull() ?: 48000
    }

    fun getOptimalFramesPerBuffer(): Int {
        return audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER)
            ?.toIntOrNull() ?: 256
    }

    fun isProAudioDevice(): Boolean {
        return context.packageManager.hasSystemFeature("android.hardware.audio.pro")
    }

    fun isLowLatencyDevice(): Boolean {
        return context.packageManager.hasSystemFeature("android.hardware.audio.low_latency")
    }

    fun registerDeviceCallback(callback: AudioDeviceCallback) {
        audioManager.registerAudioDeviceCallback(callback, null)
    }

    fun unregisterDeviceCallback(callback: AudioDeviceCallback) {
        audioManager.unregisterAudioDeviceCallback(callback)
    }

    fun logDeviceInfo() {
        val devices = enumerateDevices()
        Log.i(PulpApplication.LOG_TAG, "Audio devices: ${devices.size}")
        Log.i(PulpApplication.LOG_TAG, "Optimal sample rate: ${getOptimalSampleRate()}")
        Log.i(PulpApplication.LOG_TAG, "Optimal buffer size: ${getOptimalFramesPerBuffer()}")
        Log.i(PulpApplication.LOG_TAG, "Pro audio: ${isProAudioDevice()}")
        Log.i(PulpApplication.LOG_TAG, "Low latency: ${isLowLatencyDevice()}")
    }
}
