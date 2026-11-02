package com.pulp.input

import android.hardware.Sensor
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.kotlin.any
import org.mockito.kotlin.argumentCaptor
import org.mockito.kotlin.eq
import org.mockito.kotlin.mock
import org.mockito.kotlin.never
import org.mockito.kotlin.times
import org.mockito.kotlin.verify
import org.mockito.kotlin.whenever

class MotionControlSourceTest {
    @Test
    fun normalizeTiltMapsGravityRangeAndRejectsMalformedInput() {
        assertEquals(0.0f, MotionControlSource.normalizeTilt(floatArrayOf(0.0f, -9.80665f))!!, 1e-6f)
        assertEquals(0.5f, MotionControlSource.normalizeTilt(floatArrayOf(0.0f, 0.0f))!!, 1e-6f)
        assertEquals(1.0f, MotionControlSource.normalizeTilt(floatArrayOf(0.0f, 9.80665f))!!, 1e-6f)
        assertEquals(1.0f, MotionControlSource.normalizeTilt(floatArrayOf(0.0f, 100.0f))!!, 0.0f)
        assertTrue(MotionControlSource.normalizeTilt(floatArrayOf(0.0f)) == null)
        assertTrue(MotionControlSource.normalizeTilt(floatArrayOf(0.0f, Float.NaN)) == null)
    }

    @Test
    fun missingSensorDoesNotRegister() {
        val manager = mock<SensorManager>()
        whenever(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)).thenReturn(null)
        val source = MotionControlSource(manager, mock<Handler>()) { }

        assertFalse(source.start())
        verify(manager, never()).registerListener(
            any<SensorEventListener>(), any<Sensor>(), any<Int>(), any<Handler>(),
        )
    }

    @Test
    fun samplesAreSequencedAndStopPublishesLoss() {
        val manager = mock<SensorManager>()
        val sensor = mock<Sensor>()
        whenever(sensor.type).thenReturn(Sensor.TYPE_ACCELEROMETER)
        whenever(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)).thenReturn(sensor)
        whenever(
            manager.registerListener(
                any<SensorEventListener>(), eq(sensor), eq(SensorManager.SENSOR_DELAY_GAME),
                any<Handler>(),
            ),
        )
            .thenReturn(true)
        val frames = mutableListOf<MotionControlFrame>()
        val source = MotionControlSource(manager, mock<Handler>(), frames::add)

        assertTrue(source.start())
        source.submitAccelerometerSample(floatArrayOf(0.0f, 0.0f), 123L)
        source.stop()
        source.stop()

        assertEquals(2, frames.size)
        assertEquals(MotionControlFrame(1, 0.5f, 1.0f, true, 123L, epoch = 1L), frames[0])
        assertEquals(MotionControlFrame(2, 0.0f, 0.0f, false, 0L, epoch = 1L), frames[1])
        verify(manager, times(1)).unregisterListener(any<SensorEventListener>(), eq(sensor))
    }

    @Test
    fun malformedSamplePublishesInvalidFrameWithoutRefreshingWithValidData() {
        val manager = mock<SensorManager>()
        val sensor = mock<Sensor>()
        whenever(sensor.type).thenReturn(Sensor.TYPE_ACCELEROMETER)
        whenever(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)).thenReturn(sensor)
        whenever(
            manager.registerListener(
                any<SensorEventListener>(), eq(sensor), eq(SensorManager.SENSOR_DELAY_GAME),
                any<Handler>(),
            ),
        )
            .thenReturn(true)
        val frames = mutableListOf<MotionControlFrame>()
        val source = MotionControlSource(manager, mock<Handler>(), frames::add)
        assertTrue(source.start())

        source.submitAccelerometerSample(floatArrayOf(0.0f, Float.NaN), 123L)

        assertEquals(MotionControlFrame(1, 0.0f, 0.0f, false, 0L, epoch = 1L), frames.single())
        assertEquals(Status.UNAVAILABLE, frames.single().status)
    }

    @Test
    fun restartAdvancesEpochAndStopIsUnavailable() {
        val manager = mock<SensorManager>()
        val sensor = mock<Sensor>()
        whenever(sensor.type).thenReturn(Sensor.TYPE_ACCELEROMETER)
        whenever(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)).thenReturn(sensor)
        whenever(manager.registerListener(any<SensorEventListener>(), eq(sensor), eq(SensorManager.SENSOR_DELAY_GAME), any<Handler>())).thenReturn(true)
        val frames = mutableListOf<MotionControlFrame>()
        val source = MotionControlSource(manager, mock<Handler>(), frames::add)
        assertTrue(source.start()); source.submitAccelerometerSample(floatArrayOf(0f, 0f), 1L); source.stop()
        assertTrue(source.start()); source.submitAccelerometerSample(floatArrayOf(0f, 0f), 2L)
        assertEquals(1L, frames[0].epoch); assertEquals(1L, frames[1].epoch)
        assertEquals(Status.UNAVAILABLE, frames[1].status)
        assertEquals(2L, frames[2].epoch)
    }

    @Test
    fun staleCallbackFromEndedEpochCannotPublishAfterConcurrentStopRestart() {
        val manager = mock<SensorManager>()
        val sensor = mock<Sensor>()
        whenever(sensor.type).thenReturn(Sensor.TYPE_ACCELEROMETER)
        whenever(manager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)).thenReturn(sensor)
        whenever(manager.registerListener(any<SensorEventListener>(), eq(sensor), eq(SensorManager.SENSOR_DELAY_GAME), any<Handler>())).thenReturn(true)
        val callbacks = argumentCaptor<SensorEventListener>()
        val frames = mutableListOf<MotionControlFrame>()
        val source = MotionControlSource(manager, mock<Handler>(), frames::add)

        assertTrue(source.start())
        verify(manager).registerListener(callbacks.capture(), eq(sensor), eq(SensorManager.SENSOR_DELAY_GAME), any<Handler>())
        val firstEpochCallback = callbacks.firstValue
        val staleEvent = mock<android.hardware.SensorEvent>()
        whenever(staleEvent.sensor).thenReturn(sensor)
        whenever(staleEvent.values).thenReturn(floatArrayOf(0.0f, 0.0f))
        whenever(staleEvent.timestamp).thenReturn(99L)

        val releaseStaleCallback = CountDownLatch(1)
        val staleThread = Thread {
            releaseStaleCallback.await(1, TimeUnit.SECONDS)
            firstEpochCallback.onSensorChanged(staleEvent)
        }
        staleThread.start()
        source.stop()
        assertTrue(source.start())
        releaseStaleCallback.countDown()
        staleThread.join(1000)
        assertTrue(!staleThread.isAlive)

        assertEquals(1, frames.count { it.status == Status.UNAVAILABLE })
        assertEquals(0, frames.count { it.timestampNanos == 99L })
        assertEquals(2L, frames.last().epoch)
    }

}
