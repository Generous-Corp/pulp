package com.pulp

import android.app.Application
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner

class PulpApplication : Application(), LifecycleEventObserver {

    override fun onCreate() {
        super.onCreate()
        try {
            System.loadLibrary("pulp")
            nativeLoaded = true
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e(LOG_TAG, "Failed to load libpulp.so: ${e.message}")
        }
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
    }

    companion object {
        const val LOG_TAG = "Pulp"
        var nativeLoaded = false
            private set
    }

    override fun onStateChanged(source: LifecycleOwner, event: Lifecycle.Event) {
        // Process-level lifecycle — not per-activity
    }
}
