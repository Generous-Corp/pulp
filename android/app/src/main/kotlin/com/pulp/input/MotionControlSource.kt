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
)

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

    private var sensor: Sensor? = null
    private var sequence: Long = 0
    private var running = false

    /** Register the accelerometer, returning false when hardware is absent. */
    fun start(): Boolean {
        if (running) return true
        val available = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            ?: return false
        if (!sensorManager.registerListener(this, available, SensorManager.SENSOR_DELAY_GAME, handler)) {
            return false
        }
        sensor = available
        running = true
        return true
    }

    /** Stop polling and publish deterministic provider loss when active. */
    fun stop() {
        if (!running) return
        sensorManager.unregisterListener(this, sensor)
        running = false
        sensor = null
        publish(
            MotionControlFrame(
                sequence = ++sequence,
                value = 0.0f,
                confidence = 0.0f,
                valid = false,
                timestampNanos = 0L,
            ),
        )
    }

    override fun onSensorChanged(event: SensorEvent) {
        if (!running || event.sensor.type != Sensor.TYPE_ACCELEROMETER) return
        submitAccelerometerSample(event.values, event.timestamp)
    }

    override fun onAccuracyChanged(sensor: Sensor, accuracy: Int) {
        if (running && accuracy == SensorManager.SENSOR_STATUS_UNRELIABLE) {
            publish(
                MotionControlFrame(
                    sequence = ++sequence,
                    value = 0.0f,
                    confidence = 0.0f,
                    valid = false,
                    timestampNanos = 0L,
                ),
            )
        }
    }

    /** Testable/provider-facing conversion from Y-axis acceleration to 0..1. */
    internal fun submitAccelerometerSample(values: FloatArray, timestampNanos: Long) {
        val normalized = normalizeTilt(values)
        publish(
            MotionControlFrame(
                sequence = ++sequence,
                value = normalized ?: 0.0f,
                confidence = if (normalized == null) 0.0f else 1.0f,
                valid = normalized != null,
                timestampNanos = if (normalized == null) 0L else timestampNanos,
            ),
        )
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
