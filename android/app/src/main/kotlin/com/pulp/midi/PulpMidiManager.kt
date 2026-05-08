package com.pulp.midi

import android.content.Context
import android.media.midi.*
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import com.pulp.PulpApplication

/**
 * Kotlin-side MIDI device management.
 * Handles device discovery, port open/close, and data routing to C++.
 * MIDI data flows to the C++ audio thread via a lock-free SPSC queue.
 */
class PulpMidiManager(private val context: Context) {

    private val midiManager = context.getSystemService(MidiManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private val openDevices = mutableMapOf<Int, MidiDevice>()
    private val openInputPorts = mutableMapOf<Int, MidiOutputPort>()  // "input" to us = "output" from Android
    private val openOutputPorts = mutableMapOf<Int, MidiInputPort>()

    init {
        midiManager?.registerDeviceCallback(object : MidiManager.DeviceCallback() {
            override fun onDeviceAdded(device: MidiDeviceInfo) {
                val name = device.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
                val transport = getTransportType(device)
                Log.i(TAG, "MIDI device added: $name (id=${device.id}, transport=$transport)")
                nativeOnDeviceAdded(device.id, name, transport)
            }

            override fun onDeviceRemoved(device: MidiDeviceInfo) {
                val name = device.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "Unknown"
                Log.i(TAG, "MIDI device removed: $name (id=${device.id})")
                closeDevice(device.id)
                nativeOnDeviceRemoved(device.id)
            }
        }, handler)
    }

    /**
     * Open a MIDI device and start receiving data from all its output ports.
     * Called from C++ via JNI when the user selects a MIDI input source.
     */
    fun openDevice(deviceId: Int) {
        val devices = midiManager?.devices ?: return
        val info = devices.find { it.id == deviceId } ?: run {
            Log.e(TAG, "MIDI device $deviceId not found")
            return
        }

        midiManager?.openDevice(info, { device ->
            if (device == null) {
                Log.e(TAG, "Failed to open MIDI device $deviceId")
                return@openDevice
            }

            openDevices[deviceId] = device
            Log.i(TAG, "MIDI device $deviceId opened")

            // Open all output ports (data FROM the device TO us)
            for (portInfo in info.ports) {
                if (portInfo.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                    val port = device.openOutputPort(portInfo.portNumber)
                    if (port != null) {
                        port.connect(MidiReceiver(deviceId, portInfo.portNumber))
                        openInputPorts[deviceId * 100 + portInfo.portNumber] = port
                        Log.i(TAG, "Opened output port ${portInfo.portNumber} on device $deviceId")
                    }
                }
            }
        }, handler)
    }

    /**
     * Close a MIDI device and all its ports.
     */
    fun closeDevice(deviceId: Int) {
        // Close all ports for this device
        val keysToRemove = openInputPorts.keys.filter { it / 100 == deviceId }
        for (key in keysToRemove) {
            openInputPorts.remove(key)?.close()
        }
        val outKeysToRemove = openOutputPorts.keys.filter { it / 100 == deviceId }
        for (key in outKeysToRemove) {
            openOutputPorts.remove(key)?.close()
        }
        openDevices.remove(deviceId)?.close()
        Log.i(TAG, "MIDI device $deviceId closed")
    }

    /**
     * Send MIDI data to a device's input port.
     * Called from C++ when the engine wants to send MIDI output.
     */
    fun sendMidi(deviceId: Int, portNumber: Int, data: ByteArray, offset: Int, count: Int, timestamp: Long) {
        val key = deviceId * 100 + portNumber
        val port = openOutputPorts[key]
        if (port != null) {
            port.send(data, offset, count, timestamp)
        } else {
            // Try to open the input port on-demand
            val device = openDevices[deviceId] ?: return
            val newPort = device.openInputPort(portNumber) ?: return
            openOutputPorts[key] = newPort
            newPort.send(data, offset, count, timestamp)
        }
    }

    /**
     * Get transport type: UMP (MIDI 2.0) or bytestream (MIDI 1.0).
     */
    fun getTransportType(device: MidiDeviceInfo): Int {
        return resolveTransportType(Build.VERSION.SDK_INT, device)
    }

    /**
     * List all currently connected MIDI devices.
     */
    fun getDevices(): List<MidiDeviceInfo> {
        return midiManager?.devices?.toList() ?: emptyList()
    }

    fun destroy() {
        for ((id, _) in openDevices.toMap()) {
            closeDevice(id)
        }
    }

    // ── MIDI Receiver ─────────────────────────────────────────────────────
    // Receives MIDI bytes from a device port and forwards to C++ via JNI.
    // This callback runs on the MIDI thread — we push data to C++ which
    // enqueues it on a lock-free SPSC queue for the audio thread.

    private inner class MidiReceiver(
        private val deviceId: Int,
        private val portNumber: Int
    ) : android.media.midi.MidiReceiver() {

        override fun onSend(data: ByteArray, offset: Int, count: Int, timestamp: Long) {
            nativeOnMidiReceived(deviceId, portNumber, data, offset, count, timestamp)
        }
    }

    // ── Native Methods ────────────────────────────────────────────────────

    private external fun nativeOnDeviceAdded(id: Int, name: String, transport: Int)
    private external fun nativeOnDeviceRemoved(id: Int)
    private external fun nativeOnMidiReceived(
        deviceId: Int, portNumber: Int,
        data: ByteArray, offset: Int, count: Int, timestamp: Long
    )

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
        const val TRANSPORT_BYTESTREAM = 1
        const val TRANSPORT_UMP = 2

        /**
         * SDK level at which `MidiDeviceInfo.defaultProtocol` (UMP /
         * MIDI 2.0 negotiation) became available — Android 13.
         */
        const val UMP_PROTOCOL_MIN_SDK = 33

        /**
         * Transport-resolution helper, separated from `Build.VERSION.SDK_INT`
         * so unit tests can drive BOTH branches deterministically.
         *
         * Codex P2 on PR #1275 — the previous test derived
         * `expectedTransport` from the same global SDK_INT branch
         * the production code consulted, so any single test run
         * exercised at most one side of the API gate. The split
         * here lets the test assert pre-33 and 33+ behaviour in
         * the same JVM run, regardless of the host Android level
         * reported by Robolectric / mock SDK config.
         */
        @JvmStatic
        fun resolveTransportType(sdkInt: Int, device: MidiDeviceInfo): Int {
            return if (sdkInt >= UMP_PROTOCOL_MIN_SDK) {
                device.defaultProtocol  // PROTOCOL_UMP or PROTOCOL_BYTESTREAM
            } else {
                TRANSPORT_BYTESTREAM
            }
        }
    }
}
