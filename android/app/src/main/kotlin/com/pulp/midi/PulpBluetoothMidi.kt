package com.pulp.midi

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import androidx.core.app.ActivityCompat
import com.pulp.PulpApplication
import java.lang.ref.WeakReference

internal interface BluetoothMidiBackend {
    interface Listener {
        fun onPeripheralFound(id: String, name: String, rssi: Int, paired: Boolean)
        fun onConnected(id: String, name: String, hasInput: Boolean, hasOutput: Boolean)
        fun onConnectionFailed(id: String, error: Int)
        fun onDisconnected(id: String)
        fun onScanStopped(error: Int)
        fun onMidiReceived(id: String, bytes: ByteArray, offset: Int, count: Int, timestamp: Long)
    }

    fun isAvailable(): Boolean
    fun startScan(listener: Listener): Boolean
    fun stopScan()
    fun connect(id: String, listener: Listener): Boolean
    fun disconnect(id: String, listener: Listener)
    fun send(id: String, bytes: ByteArray): Boolean
    fun close(listener: Listener)
    fun onActivityResumed(activity: Activity)
    fun onActivityPaused(activity: Activity)
}

class PulpBluetoothMidi internal constructor(
    private val backend: BluetoothMidiBackend,
) : BluetoothMidiBackend.Listener {

    constructor(context: Context) : this(AndroidBluetoothMidiBackend(context))

    init {
        nativeInstallBridge()
    }

    private fun isNativeAvailable(): Boolean = backend.isAvailable()

    private fun startNativeScan(): Boolean = backend.startScan(this)

    private fun stopNativeScan() = backend.stopScan()

    private fun connectNative(id: String): Boolean = backend.connect(id, this)

    private fun disconnectNative(id: String) = backend.disconnect(id, this)

    private fun sendNative(id: String, bytes: ByteArray): Boolean = backend.send(id, bytes)

    fun close() = backend.close(this)

    fun onActivityResumed(activity: Activity) = backend.onActivityResumed(activity)

    fun onActivityPaused(activity: Activity) = backend.onActivityPaused(activity)

    override fun onPeripheralFound(id: String, name: String, rssi: Int, paired: Boolean) {
        nativeOnPeripheralFound(id, name, rssi, paired)
    }

    override fun onConnected(id: String, name: String, hasInput: Boolean, hasOutput: Boolean) {
        nativeOnConnected(id, name, hasInput, hasOutput)
    }

    override fun onConnectionFailed(id: String, error: Int) {
        nativeOnConnectionFailed(id, error)
    }

    override fun onDisconnected(id: String) {
        nativeOnDisconnected(id)
    }

    override fun onScanStopped(error: Int) {
        nativeOnScanStopped(error)
    }

    override fun onMidiReceived(
        id: String,
        bytes: ByteArray,
        offset: Int,
        count: Int,
        timestamp: Long,
    ) {
        nativeOnMidiReceived(id, bytes, offset, count, timestamp)
    }

    external fun nativeBeginRegistryValidation(): Boolean
    external fun nativeConnectRegistryValidation(id: String): Boolean
    external fun nativeAttachRegistryValidationInput(id: String): Boolean
    external fun nativeRegistryValidationReceived(expected: ByteArray): Boolean
    external fun nativeRegistryValidationReceivedAt(): Double
    external fun nativeSendRegistryValidationOutput(id: String, bytes: ByteArray): Boolean
    external fun nativeDisconnectRegistryValidation(id: String)
    external fun nativeRegistryFlags(id: String): Int
    external fun nativeRegistryEventCounts(): IntArray
    external fun nativeRegistryValidationIsScanning(): Boolean

    private external fun nativeInstallBridge()
    private external fun nativeOnPeripheralFound(id: String, name: String, rssi: Int, paired: Boolean)
    private external fun nativeOnConnected(
        id: String,
        name: String,
        hasInput: Boolean,
        hasOutput: Boolean,
    )
    private external fun nativeOnConnectionFailed(id: String, error: Int)
    private external fun nativeOnDisconnected(id: String)
    private external fun nativeOnScanStopped(error: Int)
    private external fun nativeOnMidiReceived(
        id: String,
        bytes: ByteArray,
        offset: Int,
        count: Int,
        timestamp: Long,
    )

    companion object {
        const val MIDI_BLE_SERVICE_UUID = "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
        const val ERROR_PERMISSION_DENIED = 1
        const val ERROR_BLUETOOTH_OFF = 2
        const val ERROR_UNSUPPORTED = 3
        const val ERROR_PERIPHERAL_NOT_FOUND = 4
        const val ERROR_CONNECT_FAILED = 5
        const val ERROR_SERVICE_NOT_FOUND = 6
    }
}

private class AndroidBluetoothMidiBackend(context: Context) : BluetoothMidiBackend {
    private data class Connection(
        val name: String,
        val device: MidiDevice,
        val inputPorts: MutableList<MidiOutputPort>,
        val outputPortNumbers: List<Int>,
        val outputPorts: MutableMap<Int, MidiInputPort>,
    )

    private val appContext = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val stateLock = Any()
    private val midiManager = appContext.getSystemService(MidiManager::class.java)
    private val bluetoothAdapter =
        appContext.getSystemService(BluetoothManager::class.java)?.adapter
    private val discovered = mutableMapOf<String, BluetoothDevice>()
    private val connections = mutableMapOf<String, Connection>()
    private val pendingConnections = mutableSetOf<String>()
    private val cancelledConnections = mutableSetOf<String>()
    private var scannerCallback: ScanCallback? = null
    private var activeListener: BluetoothMidiBackend.Listener? = null
    private var resumedActivity = WeakReference<Activity>(null)
    private val deviceCallback = object : MidiManager.DeviceCallback() {
        override fun onDeviceRemoved(device: MidiDeviceInfo) {
            val (listener, id) = synchronized(stateLock) {
                val currentListener = activeListener ?: return
                val currentId = connections.entries
                    .firstOrNull { it.value.device.info.id == device.id }
                    ?.key
                    ?: return
                currentListener to currentId
            }
            disconnect(id, listener)
        }
    }

    init {
        midiManager?.registerDeviceCallback(deviceCallback, handler)
    }

    override fun isAvailable(): Boolean {
        if (midiManager == null || bluetoothAdapter == null || !bluetoothAdapter.isEnabled) {
            return false
        }
        return hasBluetoothPermissions()
    }

    override fun startScan(listener: BluetoothMidiBackend.Listener): Boolean {
        synchronized(stateLock) {
            if (scannerCallback != null) return true
        }
        if (!hasBluetoothPermissions()) {
            requestBluetoothPermissions()
            return false
        }
        if (!isAvailable()) return false
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return false
        activeListener = listener
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device
                synchronized(stateLock) { discovered[device.address] = device }
                listener.onPeripheralFound(
                    device.address,
                    device.name ?: device.address,
                    result.rssi,
                    device.bondState == BluetoothDevice.BOND_BONDED,
                )
            }

            override fun onScanFailed(errorCode: Int) {
                synchronized(stateLock) { scannerCallback = null }
                listener.onScanStopped(PulpBluetoothMidi.ERROR_CONNECT_FAILED)
            }
        }
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid.fromString(PulpBluetoothMidi.MIDI_BLE_SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        synchronized(stateLock) { scannerCallback = callback }
        return try {
            scanner.startScan(listOf(filter), settings, callback)
            true
        } catch (error: SecurityException) {
            synchronized(stateLock) { scannerCallback = null }
            listener.onConnectionFailed("", PulpBluetoothMidi.ERROR_PERMISSION_DENIED)
            false
        }
    }

    override fun stopScan() {
        val callback = synchronized(stateLock) {
            val current = scannerCallback ?: return
            scannerCallback = null
            current
        }
        try {
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(callback)
        } catch (error: SecurityException) {
            Log.w(PulpApplication.LOG_TAG, "BLE MIDI scan stop denied: ${error.message}")
        }
    }

    override fun connect(id: String, listener: BluetoothMidiBackend.Listener): Boolean {
        val existing = synchronized(stateLock) { connections[id] }
        if (existing != null) {
            listener.onConnected(
                id,
                existing.name,
                existing.inputPorts.isNotEmpty(),
                existing.outputPorts.isNotEmpty(),
            )
            return true
        }
        val device = synchronized(stateLock) {
            if (pendingConnections.contains(id)) return true
            discovered[id]
        }
        if (device == null) {
            listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_PERIPHERAL_NOT_FOUND)
            return false
        }
        val manager = midiManager ?: run {
            listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_UNSUPPORTED)
            return false
        }
        synchronized(stateLock) {
            activeListener = listener
            pendingConnections += id
            cancelledConnections -= id
        }
        return try {
            manager.openBluetoothDevice(device, { midiDevice ->
                if (midiDevice == null) {
                    if (!finishPending(id)) {
                        listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_CONNECT_FAILED)
                    }
                    return@openBluetoothDevice
                }
                openConnection(id, device.name ?: id, midiDevice, listener)
            }, handler)
            true
        } catch (error: SecurityException) {
            synchronized(stateLock) { pendingConnections -= id }
            listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_PERMISSION_DENIED)
            false
        }
    }

    private fun openConnection(
        id: String,
        name: String,
        device: MidiDevice,
        listener: BluetoothMidiBackend.Listener,
    ) {
        try {
            val inputPorts = mutableListOf<MidiOutputPort>()
            val outputPorts = mutableMapOf<Int, MidiInputPort>()
            for (portInfo in device.info.ports) {
                when (portInfo.type) {
                    MidiDeviceInfo.PortInfo.TYPE_OUTPUT -> {
                        val port = device.openOutputPort(portInfo.portNumber) ?: continue
                        port.connect(object : android.media.midi.MidiReceiver() {
                            override fun onSend(
                                data: ByteArray,
                                offset: Int,
                                count: Int,
                                timestamp: Long,
                            ) {
                                listener.onMidiReceived(id, data, offset, count, timestamp)
                            }
                        })
                        inputPorts += port
                    }
                    MidiDeviceInfo.PortInfo.TYPE_INPUT -> {
                        val port = device.openInputPort(portInfo.portNumber) ?: continue
                        outputPorts[portInfo.portNumber] = port
                    }
                }
            }
            if (inputPorts.isEmpty() && outputPorts.isEmpty()) {
                device.close()
                if (!finishPending(id)) {
                    listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_SERVICE_NOT_FOUND)
                }
                return
            }
            val connection = Connection(
                name,
                device,
                inputPorts,
                outputPorts.keys.toList(),
                outputPorts,
            )
            val cancelled = synchronized(stateLock) {
                pendingConnections -= id
                val wasCancelled = cancelledConnections.remove(id)
                if (!wasCancelled) {
                    connections[id] = connection
                }
                wasCancelled
            }
            if (cancelled) {
                inputPorts.forEach { it.close() }
                outputPorts.values.forEach { it.close() }
                device.close()
            } else {
                listener.onConnected(
                    id,
                    name,
                    inputPorts.isNotEmpty(),
                    outputPorts.isNotEmpty(),
                )
            }
        } catch (error: SecurityException) {
            device.close()
            if (!finishPending(id)) {
                listener.onConnectionFailed(id, PulpBluetoothMidi.ERROR_PERMISSION_DENIED)
            }
        }
    }

    private fun finishPending(id: String): Boolean = synchronized(stateLock) {
        pendingConnections -= id
        cancelledConnections.remove(id)
    }

    override fun disconnect(id: String, listener: BluetoothMidiBackend.Listener) {
        val connection = synchronized(stateLock) {
            if (pendingConnections.remove(id)) cancelledConnections += id
            connections.remove(id)
        }
        if (connection != null) {
            try {
                synchronized(stateLock) {
                    connection.inputPorts.forEach { runCatching { it.close() } }
                    connection.outputPorts.values.forEach { runCatching { it.close() } }
                    runCatching { connection.device.close() }
                }
            } finally {
                listener.onDisconnected(id)
            }
        } else {
            listener.onDisconnected(id)
        }
    }

    override fun send(id: String, bytes: ByteArray): Boolean {
        synchronized(stateLock) {
            val connection = connections[id] ?: return false
            val portNumber = connection.outputPortNumbers.firstOrNull() ?: return false
            val port = connection.outputPorts[portNumber] ?: return false
            return try {
                port.send(bytes, 0, bytes.size, System.nanoTime())
                true
            } catch (error: Exception) {
                Log.e(PulpApplication.LOG_TAG, "BLE MIDI send failed: ${error.message}")
                false
            }
        }
    }

    override fun close(listener: BluetoothMidiBackend.Listener) {
        stopScan()
        val ids = synchronized(stateLock) {
            (connections.keys + pendingConnections).toList()
        }
        ids.forEach { disconnect(it, listener) }
        midiManager?.unregisterDeviceCallback(deviceCallback)
        synchronized(stateLock) { activeListener = null }
    }

    override fun onActivityResumed(activity: Activity) {
        synchronized(stateLock) { resumedActivity = WeakReference(activity) }
    }

    override fun onActivityPaused(activity: Activity) {
        synchronized(stateLock) {
            if (resumedActivity.get() === activity) resumedActivity.clear()
        }
    }

    private fun requestBluetoothPermissions() {
        if (Build.VERSION.SDK_INT < 31) return
        val activity = synchronized(stateLock) { resumedActivity.get() } ?: return
        ActivityCompat.requestPermissions(
            activity,
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT),
            BLUETOOTH_PERMISSION_REQUEST,
        )
    }

    private fun hasBluetoothPermissions(): Boolean {
        val permissions = if (Build.VERSION.SDK_INT >= 31) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        return permissions.all {
            ContextCompat.checkSelfPermission(appContext, it) == PackageManager.PERMISSION_GRANTED
        }
    }

    private companion object {
        const val BLUETOOTH_PERMISSION_REQUEST = 0x5042
    }
}
