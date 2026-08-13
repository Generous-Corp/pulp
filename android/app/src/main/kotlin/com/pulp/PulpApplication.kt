package com.pulp

import android.app.Application
import android.app.Activity
import android.app.Application.ActivityLifecycleCallbacks
import android.os.Bundle
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.pulp.midi.PulpBluetoothMidi
import com.pulp.midi.PulpMidiManager

class PulpApplication : Application(), LifecycleEventObserver {

    override fun onCreate() {
        super.onCreate()
        try {
            System.loadLibrary("pulp")
            nativeLoaded = true
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e(LOG_TAG, "Failed to load libpulp.so: ${e.message}")
        }
        if (nativeLoaded) {
            // Wire the C++ AAssetManager bridge so native code can read
            // bundled assets (e.g., synth_ui.js) from the APK.
            try {
                PulpFileProvider(this).init()
            } catch (e: Throwable) {
                android.util.Log.e(LOG_TAG, "PulpFileProvider init failed: ${e.message}")
            }
            // Start MIDI device discovery.
            // PulpMidiManager registers for MidiManager device callbacks
            // and forwards received bytes to C++ via nativeOnMidiReceived.
            try {
                midiManager = PulpMidiManager(this)
            } catch (e: Throwable) {
                android.util.Log.e(LOG_TAG, "PulpMidiManager init failed: ${e.message}")
            }
            try {
                bluetoothMidi = PulpBluetoothMidi(this)
            } catch (e: Throwable) {
                android.util.Log.e(LOG_TAG, "PulpBluetoothMidi init failed: ${e.message}")
            }
        }
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
        registerActivityLifecycleCallbacks(object : ActivityLifecycleCallbacks {
            override fun onActivityResumed(activity: Activity) {
                bluetoothMidi?.onActivityResumed(activity)
            }

            override fun onActivityCreated(activity: Activity, state: Bundle?) = Unit
            override fun onActivityStarted(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) {
                bluetoothMidi?.onActivityPaused(activity)
            }
            override fun onActivityStopped(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, state: Bundle) = Unit
            override fun onActivityDestroyed(activity: Activity) {
                bluetoothMidi?.onActivityPaused(activity)
            }
        })
    }

    companion object {
        const val LOG_TAG = "Pulp"
        var nativeLoaded = false
            private set
        var midiManager: PulpMidiManager? = null
            private set
        var bluetoothMidi: PulpBluetoothMidi? = null
            private set
    }

    override fun onStateChanged(source: LifecycleOwner, event: Lifecycle.Event) {
        // Process-level lifecycle — not per-activity
    }
}
