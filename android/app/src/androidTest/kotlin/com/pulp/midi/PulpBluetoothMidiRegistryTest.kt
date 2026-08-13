package com.pulp.midi

import android.app.Activity
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PulpBluetoothMidiRegistryTest {
    @Test
    fun fakePeripheralPublishesRoutesAndRemovesRegistryPorts() {
        System.loadLibrary("pulp_ble_midi_validation")
        val id = "AA:BB:CC:DD:EE:FF"
        val input = byteArrayOf(0x90.toByte(), 0x3c, 0x64)
        val output = byteArrayOf(0x80.toByte(), 0x3c, 0x00)
        val backend = FakeBackend(id)
        val bridge = PulpBluetoothMidi(backend)

        assertTrue(bridge.nativeBeginRegistryValidation())
        assertTrue(bridge.nativeRegistryValidationIsScanning())
        backend.stopScanFromPlatform()
        assertEquals(false, bridge.nativeRegistryValidationIsScanning())
        assertTrue(bridge.nativeBeginRegistryValidation())
        assertTrue(bridge.nativeConnectRegistryValidation(id))
        assertEquals(3, bridge.nativeRegistryFlags(id))
        assertArrayEquals(intArrayOf(1, 0), bridge.nativeRegistryEventCounts())

        assertTrue(bridge.nativeAttachRegistryValidationInput(id))
        backend.emit(input)
        assertTrue(bridge.nativeRegistryValidationReceived(input))
        assertTrue(bridge.nativeRegistryValidationReceivedAt() >= 0.0)
        assertTrue(bridge.nativeRegistryValidationReceivedAt() < 1.0)
        val secondInput = byteArrayOf(0x90.toByte(), 0x40, 0x7f)
        backend.emit(input + secondInput)
        assertTrue(bridge.nativeRegistryValidationReceived(secondInput))
        val runningStatusInput = byteArrayOf(0x90.toByte(), 0x41, 0x70, 0x42, 0x71)
        backend.emit(runningStatusInput)
        assertTrue(
            bridge.nativeRegistryValidationReceived(
                byteArrayOf(0x90.toByte(), 0x42, 0x71),
            ),
        )
        val sysex = byteArrayOf(0xf0.toByte(), 0x01, 0x02, 0xf7.toByte())
        backend.emit(sysex.copyOfRange(0, 2))
        backend.emit(sysex.copyOfRange(2, 4))
        assertTrue(bridge.nativeRegistryValidationReceived(sysex))

        assertTrue(bridge.nativeSendRegistryValidationOutput(id, output))
        assertArrayEquals(output, backend.lastWrite)
        val timingClock = byteArrayOf(0xf8.toByte())
        assertTrue(bridge.nativeSendRegistryValidationOutput(id, timingClock))
        assertArrayEquals(timingClock, backend.lastWrite)
        val songSelect = byteArrayOf(0xf3.toByte(), 0x02)
        assertTrue(bridge.nativeSendRegistryValidationOutput(id, songSelect))
        assertArrayEquals(songSelect, backend.lastWrite)

        bridge.nativeDisconnectRegistryValidation(id)
        assertEquals(0, bridge.nativeRegistryFlags(id))
        assertArrayEquals(intArrayOf(1, 1), bridge.nativeRegistryEventCounts())

        assertTrue(bridge.nativeBeginRegistryValidation())
        backend.holdConnection = true
        assertTrue(bridge.nativeConnectRegistryValidation(id))
        backend.failPendingConnection()
        backend.holdConnection = false
        assertTrue(bridge.nativeConnectRegistryValidation(id))
        assertEquals(3, bridge.nativeRegistryFlags(id))
        bridge.nativeDisconnectRegistryValidation(id)
        assertEquals(0, bridge.nativeRegistryFlags(id))
        bridge.close()
    }

    private class FakeBackend(private val id: String) : BluetoothMidiBackend {
        private var listener: BluetoothMidiBackend.Listener? = null
        var lastWrite: ByteArray = byteArrayOf()
            private set
        var holdConnection = false

        override fun isAvailable(): Boolean = true

        override fun startScan(listener: BluetoothMidiBackend.Listener): Boolean {
            this.listener = listener
            listener.onPeripheralFound(id, "Fake BLE MIDI", -42, true)
            return true
        }

        override fun stopScan() = Unit

        override fun connect(id: String, listener: BluetoothMidiBackend.Listener): Boolean {
            this.listener = listener
            if (!holdConnection) listener.onConnected(id, "Fake BLE MIDI", true, true)
            return true
        }

        override fun disconnect(id: String, listener: BluetoothMidiBackend.Listener) {
            listener.onDisconnected(id)
        }

        override fun send(id: String, bytes: ByteArray): Boolean {
            lastWrite = bytes.copyOf()
            return true
        }

        override fun close(listener: BluetoothMidiBackend.Listener) = Unit

        override fun onActivityResumed(activity: Activity) = Unit

        override fun onActivityPaused(activity: Activity) = Unit

        fun emit(bytes: ByteArray) {
            listener?.onMidiReceived(id, bytes, 0, bytes.size, System.nanoTime())
        }

        fun failPendingConnection() {
            listener?.onConnectionFailed(id, PulpBluetoothMidi.ERROR_CONNECT_FAILED)
        }

        fun stopScanFromPlatform() {
            listener?.onScanStopped(PulpBluetoothMidi.ERROR_CONNECT_FAILED)
        }
    }
}
