package com.pulp.midi

import android.content.Context
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiInputPort
import android.media.midi.MidiManager
import android.media.midi.MidiOutputPort
import android.os.Build
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.mockito.kotlin.any
import org.mockito.kotlin.doAnswer
import org.mockito.kotlin.eq
import org.mockito.kotlin.mock
import org.mockito.kotlin.never
import org.mockito.kotlin.times
import org.mockito.kotlin.verify
import org.mockito.kotlin.whenever

class PulpMidiManagerTest {
    @Test
    fun registersDeviceCallbackWhenMidiServiceIsAvailable() {
        val midiManager = mock<MidiManager>()
        val manager = PulpMidiManager(contextWith(midiManager))

        assertTrue(manager.getDevices().isEmpty())
        verify(midiManager).registerDeviceCallback(any(), any())
    }

    @Test
    fun getDevicesReturnsEmptyWhenMidiServiceIsUnavailable() {
        val manager = PulpMidiManager(contextWith(null))

        assertTrue(manager.getDevices().isEmpty())
    }

    @Test
    fun getDevicesReturnsConnectedMidiDevices() {
        val midiManager = mock<MidiManager>()
        val firstDevice = mock<MidiDeviceInfo>()
        val secondDevice = mock<MidiDeviceInfo>()
        whenever(midiManager.devices).thenReturn(arrayOf(firstDevice, secondDevice))
        val manager = PulpMidiManager(contextWith(midiManager))

        assertEquals(listOf(firstDevice, secondDevice), manager.getDevices())
    }

    @Test
    fun openDeviceConnectsOutputPortsAndDestroyClosesThem() {
        val midiManager = mock<MidiManager>()
        val deviceInfo = mock<MidiDeviceInfo>()
        val outputPortInfo = mock<MidiDeviceInfo.PortInfo>()
        val inputPortInfo = mock<MidiDeviceInfo.PortInfo>()
        val device = mock<MidiDevice>()
        val outputPort = mock<MidiOutputPort>()
        whenever(midiManager.devices).thenReturn(arrayOf(deviceInfo))
        whenever(deviceInfo.id).thenReturn(42)
        whenever(deviceInfo.ports).thenReturn(arrayOf(outputPortInfo, inputPortInfo))
        whenever(outputPortInfo.type).thenReturn(MidiDeviceInfo.PortInfo.TYPE_OUTPUT)
        whenever(outputPortInfo.portNumber).thenReturn(7)
        whenever(inputPortInfo.type).thenReturn(MidiDeviceInfo.PortInfo.TYPE_INPUT)
        whenever(inputPortInfo.portNumber).thenReturn(3)
        whenever(device.openOutputPort(7)).thenReturn(outputPort)
        openDeviceImmediately(midiManager, deviceInfo, device)
        val manager = PulpMidiManager(contextWith(midiManager))

        manager.openDevice(42)
        manager.destroy()

        verify(outputPort).connect(any())
        verify(device, never()).openOutputPort(3)
        verify(outputPort).close()
        verify(device).close()
    }

    @Test
    fun openDeviceIgnoresUnknownDeviceId() {
        val midiManager = mock<MidiManager>()
        val deviceInfo = mock<MidiDeviceInfo>()
        whenever(midiManager.devices).thenReturn(arrayOf(deviceInfo))
        whenever(deviceInfo.id).thenReturn(7)
        val manager = PulpMidiManager(contextWith(midiManager))

        manager.openDevice(99)

        verify(midiManager, never()).openDevice(any(), any(), any())
    }

    @Test
    fun sendMidiOpensInputPortOnDemandAndReusesIt() {
        val midiManager = mock<MidiManager>()
        val deviceInfo = mock<MidiDeviceInfo>()
        val device = mock<MidiDevice>()
        val inputPort = mock<MidiInputPort>()
        val payload = byteArrayOf(0x90.toByte(), 0x40, 0x7f)
        whenever(midiManager.devices).thenReturn(arrayOf(deviceInfo))
        whenever(deviceInfo.id).thenReturn(42)
        whenever(deviceInfo.ports).thenReturn(emptyArray())
        whenever(device.openInputPort(2)).thenReturn(inputPort)
        openDeviceImmediately(midiManager, deviceInfo, device)
        val manager = PulpMidiManager(contextWith(midiManager))

        manager.openDevice(42)
        manager.sendMidi(42, 2, payload, 0, payload.size, 123L)
        manager.sendMidi(42, 2, payload, 1, 2, 456L)

        verify(device, times(1)).openInputPort(2)
        verify(inputPort).send(payload, 0, payload.size, 123L)
        verify(inputPort).send(payload, 1, 2, 456L)
    }

    @Test
    fun getTransportTypeUsesPlatformProtocolOnlyOnAndroid13AndNewer() {
        val midiManager = mock<MidiManager>()
        val deviceInfo = mock<MidiDeviceInfo>()
        whenever(deviceInfo.defaultProtocol).thenReturn(PulpMidiManager.TRANSPORT_UMP)
        val manager = PulpMidiManager(contextWith(midiManager))

        val expectedTransport = if (Build.VERSION.SDK_INT >= 33) {
            PulpMidiManager.TRANSPORT_UMP
        } else {
            PulpMidiManager.TRANSPORT_BYTESTREAM
        }

        assertEquals(expectedTransport, manager.getTransportType(deviceInfo))
    }

    private fun contextWith(midiManager: MidiManager?): Context {
        val context = mock<Context>()
        whenever(context.getSystemService(MidiManager::class.java)).thenReturn(midiManager)
        return context
    }

    private fun openDeviceImmediately(
        midiManager: MidiManager,
        deviceInfo: MidiDeviceInfo,
        device: MidiDevice
    ) {
        doAnswer { invocation ->
            invocation.getArgument<MidiManager.OnDeviceOpenedListener>(1).onDeviceOpened(device)
            null
        }.whenever(midiManager).openDevice(eq(deviceInfo), any(), any())
    }
}
