package com.pulp.midi

import android.media.midi.MidiDeviceService
import android.media.midi.MidiDeviceStatus
import android.media.midi.MidiReceiver
import android.util.Log
import com.pulp.PulpApplication

/**
 * Virtual MIDI device service — makes Pulp visible as a MIDI device
 * to other Android apps. Enables inter-app MIDI routing.
 *
 * Declared in AndroidManifest.xml with BIND_MIDI_DEVICE_SERVICE permission.
 * The device info (ports, name) is defined in res/xml/midi_device_info.xml.
 */
class PulpMidiService : MidiDeviceService() {

    override fun onGetInputPortReceivers(): Array<MidiReceiver> {
        // Input ports: data FROM other apps TO Pulp
        return arrayOf(PulpVirtualInputReceiver())
    }

    override fun onDeviceStatusChanged(status: MidiDeviceStatus) {
        Log.i(TAG, "Virtual MIDI device status changed: " +
                "inputOpen=${status.isInputPortOpen(0)}")
    }

    override fun onClose() {
        Log.i(TAG, "Virtual MIDI service closed")
    }

    /**
     * Receives MIDI data from other apps connected to our virtual input port.
     * Routes to C++ via the same JNI path as hardware MIDI.
     */
    private inner class PulpVirtualInputReceiver : MidiReceiver() {
        override fun onSend(data: ByteArray, offset: Int, count: Int, timestamp: Long) {
            if (PulpApplication.nativeLoaded) {
                nativeOnVirtualMidiReceived(data, offset, count, timestamp)
            }
        }
    }

    private external fun nativeOnVirtualMidiReceived(
        data: ByteArray, offset: Int, count: Int, timestamp: Long
    )

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
    }
}
