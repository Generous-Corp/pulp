package com.pulp.render

import android.content.Context
import android.util.Log
import com.pulp.PulpApplication

/**
 * GPU driver blocklist and crash recovery for Vulkan.
 *
 * Android Vulkan drivers (especially older Mali and Adreno) can crash during
 * vkCreateDevice or shader compilation. Strategy:
 * 1. Check blocklist before init
 * 2. Set crash flag BEFORE Vulkan init attempt
 * 3. Clear flag after first successful frame
 * 4. If app crashes → next launch sees flag → permanent OpenGL ES fallback
 */
object GpuDriverPolicy {

    private const val PREFS_NAME = "pulp_gpu"
    private const val KEY_VULKAN_CRASHED = "vulkan_crashed"
    private const val KEY_FORCE_GLES = "force_gles"

    data class GpuDriverEntry(val gpuName: String, val maxDriverVersion: Int)

    // Known-bad driver combinations — updated via app update as field data arrives
    private val VULKAN_BLOCKLIST = listOf(
        GpuDriverEntry("Mali-G72", maxDriverVersion = 28),
        GpuDriverEntry("Adreno (TM) 505", maxDriverVersion = 512),
    )

    fun shouldUseVulkan(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

        if (prefs.getBoolean(KEY_VULKAN_CRASHED, false)) {
            Log.w(TAG, "Vulkan previously crashed — using OpenGL ES fallback")
            return false
        }
        if (prefs.getBoolean(KEY_FORCE_GLES, false)) {
            Log.i(TAG, "OpenGL ES forced by user preference")
            return false
        }

        // TODO: Check blocklist against actual GPU info from nativeGetGpuName()
        return true
    }

    /**
     * Set crash flag BEFORE attempting Vulkan init.
     * If the app crashes during init, the flag persists → next launch uses GLES.
     */
    fun markVulkanAttempt(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, true).apply()
    }

    /**
     * Clear crash flag after first successful Vulkan frame.
     */
    fun markVulkanSuccess(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, false).apply()
    }

    /**
     * Force OpenGL ES for this device (user choice).
     */
    fun forceOpenGLES(context: Context, force: Boolean) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_FORCE_GLES, force).apply()
    }

    /**
     * Reset crash flag — allows retrying Vulkan after an OS update.
     */
    fun resetCrashFlag(context: Context) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_VULKAN_CRASHED, false).apply()
    }

    private const val TAG = PulpApplication.LOG_TAG
}
