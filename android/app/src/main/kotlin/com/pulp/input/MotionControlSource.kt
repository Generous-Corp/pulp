package com.pulp.input

import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler

/**
 * One application-owned frame emitted by [MotionControlSource].
 *
 * The consumer can forward this frame through the existing native queue,
 * OSC/IPC, or an application-owned mapper. Pulp core does not interpret the
 * value and this class never calls into audio code from a sensor callback.
 */
data class MotionControlFrame(
    val sequence: Long,
    val value: Float,
    val confidence: Float,
    val valid: Boolean,
    val timestampNanos: Long,
    val sourceId: String = "android.motion.accelerometer",
    val epoch: Long = 0L,
    val status: Status = if (valid) Status.ACCEPTED else Status.UNAVAILABLE,
)

enum class Status { ACCEPTED, PAUSED, DENIED, UNAVAILABLE }

/**
 * Application-owned Android accelerometer provider.
 *
 * Sensor polling is registered on the supplied [Handler]. Call [stop] from
 * lifecycle teardown; it unregisters the listener and emits one invalid frame
 * so a consumer can apply its normal tracking-loss policy. The provider has no
 * mandatory Pulp or ML dependency.
 */
class MotionControlSource(
    private val sensorManager: SensorManager,
    private val handler: Handler,
    private val publish: (MotionControlFrame) -> Unit,
) : SensorEventListener {

    private val stateLock = Any()
    private var sensor: Sensor? = null
    private var callback: SensorEventListener? = null
    private var sequence: Long = 0
    private var running = false
    private var epoch: Long = 0

    /** Register the accelerometer, returning false when hardware is absent. */
    fun start(): Boolean {
        synchronized(stateLock) {
            if (running) return true
            val available = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
                ?: return false
            val nextEpoch = epoch + 1
            val epochCallback = EpochListener(nextEpoch)
            if (!sensorManager.registerListener(epochCallback, available, SensorManager.SENSOR_DELAY_GAME, handler)) {
                return false
            }
            sensor = available
            callback = epochCallback
            epoch = nextEpoch
            running = true
            return true
        }
    }

    /** Stop polling and publish deterministic provider loss when active. */
    fun stop() {
        synchronized(stateLock) {
            if (!running) return
            val activeSensor = sensor
            val activeCallback = callback
            if (activeSensor != null && activeCallback != null) {
                sensorManager.unregisterListener(activeCallback, activeSensor)
            }
            running = false
            sensor = null
            callback = null
            publishUnavailableLocked()
        }
    }

    override fun onSensorChanged(event: SensorEvent) {
        synchronized(stateLock) {
            if (!running || event.sensor.type != Sensor.TYPE_ACCELEROMETER) return
            submitAccelerometerSampleLocked(event.values, event.timestamp, epoch)
        }
    }

    override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {
        synchronized(stateLock) {
            if (running && accuracy == SensorManager.SENSOR_STATUS_UNRELIABLE) {
                publishUnavailableLocked()
            }
        }
    }

    /** Testable/provider-facing conversion from Y-axis acceleration to 0..1. */
    internal fun submitAccelerometerSample(values: FloatArray, timestampNanos: Long) {
        synchronized(stateLock) {
            if (!running) return
            submitAccelerometerSampleLocked(values, timestampNanos, epoch)
        }
    }

    private fun submitAccelerometerSampleLocked(values: FloatArray, timestampNanos: Long, callbackEpoch: Long) {
        if (!running || callbackEpoch != epoch) return
        val normalized = normalizeTilt(values)
        publish(
            MotionControlFrame(
                sequence = ++sequence,
                epoch = epoch,
                value = normalized ?: 0.0f,
                confidence = if (normalized == null) 0.0f else 1.0f,
                valid = normalized != null,
                timestampNanos = if (normalized == null) 0L else timestampNanos,
                status = if (normalized == null) Status.UNAVAILABLE else Status.ACCEPTED,
            ),
        )
    }

    private fun publishUnavailableLocked() {
        publish(
            MotionControlFrame(
                sequence = ++sequence,
                epoch = epoch,
                value = 0.0f,
                confidence = 0.0f,
                valid = false,
                timestampNanos = 0L,
                status = Status.UNAVAILABLE,
            ),
        )
    }

    private inner class EpochListener(private val callbackEpoch: Long) : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            synchronized(stateLock) {
                submitAccelerometerSampleLocked(event.values, event.timestamp, callbackEpoch)
            }
        }

        override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {
            synchronized(stateLock) {
                if (running && callbackEpoch == epoch && accuracy == SensorManager.SENSOR_STATUS_UNRELIABLE) {
                    publishUnavailableLocked()
                }
            }
        }
    }

    companion object {
        private const val GRAVITY = 9.80665f

        /** Convert a finite Y-axis acceleration into a calibrated normalized value. */
        fun normalizeTilt(values: FloatArray): Float? {
            if (values.size < 2 || !values[1].isFinite()) return null
            return (((values[1] / GRAVITY) + 1.0f) * 0.5f).coerceIn(0.0f, 1.0f)
        }
    }
}
