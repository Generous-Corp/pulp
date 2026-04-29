package com.pulp.audio

import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.kotlin.any
import org.mockito.kotlin.eq
import org.mockito.kotlin.mock
import org.mockito.kotlin.verify
import org.mockito.kotlin.whenever

class PulpAudioEngineTest {
    @Test
    fun queriesAudioManagerAndPackageManagerCapabilities() {
        val context = mock<Context>()
        val audioManager = mock<AudioManager>()
        val packageManager = mock<PackageManager>()
        val inputDevice = mock<AudioDeviceInfo>()
        val outputDevice = mock<AudioDeviceInfo>()
        whenever(context.getSystemService(AudioManager::class.java)).thenReturn(audioManager)
        whenever(context.packageManager).thenReturn(packageManager)
        whenever(audioManager.getDevices(any())).thenReturn(arrayOf(inputDevice, outputDevice))
        whenever(audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)).thenReturn("96000")
        whenever(audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER))
            .thenReturn("512")
        whenever(packageManager.hasSystemFeature("android.hardware.audio.pro")).thenReturn(true)
        whenever(packageManager.hasSystemFeature("android.hardware.audio.low_latency"))
            .thenReturn(false)
        val engine = PulpAudioEngine(context)

        assertEquals(2, engine.enumerateDevices().size)
        assertEquals(96000, engine.getOptimalSampleRate())
        assertEquals(512, engine.getOptimalFramesPerBuffer())
        assertTrue(engine.isProAudioDevice())
        assertFalse(engine.isLowLatencyDevice())
    }

    @Test
    fun usesDefaultAudioConfigWhenPropertiesAreUnavailableOrMalformed() {
        val context = mock<Context>()
        val audioManager = mock<AudioManager>()
        whenever(context.getSystemService(AudioManager::class.java)).thenReturn(audioManager)
        whenever(audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE))
            .thenReturn("not-a-number")
        whenever(audioManager.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER))
            .thenReturn(null)
        val engine = PulpAudioEngine(context)

        assertEquals(48000, engine.getOptimalSampleRate())
        assertEquals(256, engine.getOptimalFramesPerBuffer())
    }

    @Test
    fun registerAndUnregisterDelegateToAudioManager() {
        val context = mock<Context>()
        val audioManager = mock<AudioManager>()
        whenever(context.getSystemService(AudioManager::class.java)).thenReturn(audioManager)
        val callback = mock<AudioDeviceCallback>()
        val engine = PulpAudioEngine(context)

        engine.registerDeviceCallback(callback)
        engine.unregisterDeviceCallback(callback)

        verify(audioManager).registerAudioDeviceCallback(eq(callback), eq(null))
        verify(audioManager).unregisterAudioDeviceCallback(callback)
    }
}
