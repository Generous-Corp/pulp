package com.pulp.audio

import android.content.ComponentName
import android.content.Context
import android.content.ServiceConnection
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.kotlin.any
import org.mockito.kotlin.argumentCaptor
import org.mockito.kotlin.doThrow
import org.mockito.kotlin.eq
import org.mockito.kotlin.mock
import org.mockito.kotlin.times
import org.mockito.kotlin.verify
import org.mockito.kotlin.whenever

class PulpAudioControllerTest {
    @Test
    fun startAudioStartsForegroundServiceAndBinds() {
        val context = mock<Context>()
        whenever(context.bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))).thenReturn(true)
        val controller = PulpAudioController(context)

        controller.startAudio(isRecording = true)

        verify(context).startForegroundService(any())
        verify(context).bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))
        assertTrue(controller.isRunning)
    }

    @Test
    fun startAudioIsIdempotentWhileRunning() {
        val context = mock<Context>()
        whenever(context.bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))).thenReturn(true)
        val controller = PulpAudioController(context)

        controller.startAudio()
        controller.startAudio()

        verify(context, times(1)).startForegroundService(any())
        verify(context, times(1)).bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))
    }

    @Test
    fun stopAudioStopsServiceAndUnbinds() {
        val context = mock<Context>()
        whenever(context.bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))).thenReturn(true)
        val controller = PulpAudioController(context)
        controller.startAudio()

        controller.stopAudio()

        verify(context).startService(any())
        verify(context).unbindService(any())
        assertFalse(controller.isRunning)
    }

    @Test
    fun stopAudioIgnoresMissingBinding() {
        val context = mock<Context>()
        whenever(context.bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))).thenReturn(true)
        doThrow(IllegalArgumentException("no binding")).whenever(context).unbindService(any())
        val controller = PulpAudioController(context)
        controller.startAudio()

        controller.stopAudio()

        assertFalse(controller.isRunning)
    }

    @Test
    fun disconnectCallbackClearsRunningState() {
        val context = mock<Context>()
        whenever(context.bindService(any(), any(), eq(Context.BIND_AUTO_CREATE))).thenReturn(true)
        val connectionCaptor = argumentCaptor<ServiceConnection>()
        val controller = PulpAudioController(context)

        controller.startAudio()
        verify(context).bindService(any(), connectionCaptor.capture(), eq(Context.BIND_AUTO_CREATE))

        connectionCaptor.firstValue.onServiceDisconnected(
            ComponentName("com.pulp.app", "com.pulp.PulpAudioService")
        )

        assertFalse(controller.isRunning)
    }
}
