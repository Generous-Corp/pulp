package com.pulp.midi

import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiManager
import android.os.Build
import android.os.ParcelUuid
import android.util.Log
import com.pulp.PulpApplication
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

/**
 * Bluetooth MIDI device discovery and pairing.
 * Uses BLE scanning with the MIDI service UUID and coroutine-based
 * connection flow (no hardcoded delays like JUCE's 2000ms timer).
 */
class PulpBluetoothMidi(private val context: Context) {

    private val midiManager = context.getSystemService(MidiManager::class.java)
    private val bluetoothAdapter = BluetoothAdapter.getDefaultAdapter()
    private var scanner: BluetoothLeScanner? = null
    private var scanCallback: ScanCallback? = null

    /**
     * Scan for BLE MIDI devices. Returns a Flow of discovered devices.
     * The flow completes when scanning is stopped via [stopScan].
     */
    fun scanForDevices(): Flow<BluetoothDevice> = callbackFlow {
        scanner = bluetoothAdapter?.bluetoothLeScanner
        if (scanner == null) {
            Log.w(TAG, "BLE scanner not available")
            close()
            return@callbackFlow
        }

        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid.fromString(MIDI_BLE_SERVICE_UUID))
            .build()

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanCallback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device
                Log.i(TAG, "BLE MIDI device found: ${device.name ?: "Unknown"} (${device.address})")
                trySend(device)
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "BLE scan failed: error=$errorCode")
                close()
            }
        }

        scanner?.startScan(listOf(filter), settings, scanCallback!!)
        Log.i(TAG, "BLE MIDI scan started")

        awaitClose {
            stopScan()
        }
    }

    /**
     * Stop the current BLE scan.
     */
    fun stopScan() {
        scanCallback?.let { cb ->
            try {
                scanner?.stopScan(cb)
                Log.i(TAG, "BLE MIDI scan stopped")
            } catch (e: Exception) {
                Log.w(TAG, "Error stopping BLE scan: ${e.message}")
            }
        }
        scanCallback = null
    }

    /**
     * Pair and open a Bluetooth MIDI device.
     * Uses suspendCancellableCoroutine instead of JUCE's hardcoded timer delays.
     * The coroutine resumes when the connection completes (success or failure).
     */
    suspend fun openBluetoothDevice(device: BluetoothDevice): MidiDevice? {
        return suspendCancellableCoroutine { continuation ->
            try {
                midiManager?.openBluetoothDevice(device, { midiDevice ->
                    if (midiDevice != null) {
                        Log.i(TAG, "BLE MIDI device opened: ${device.name}")
                    } else {
                        Log.e(TAG, "Failed to open BLE MIDI device: ${device.name}")
                    }
                    continuation.resume(midiDevice)
                }, null)
            } catch (e: SecurityException) {
                Log.e(TAG, "BLE MIDI permission denied: ${e.message}")
                continuation.resume(null)
            }

            continuation.invokeOnCancellation {
                Log.w(TAG, "BLE MIDI connection cancelled for ${device.name}")
            }
        }
    }

    companion object {
        private const val TAG = PulpApplication.LOG_TAG
        // Standard BLE MIDI service UUID
        const val MIDI_BLE_SERVICE_UUID = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
    }
}
